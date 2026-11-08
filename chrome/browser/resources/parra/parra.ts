// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/**
 * chrome://parra -- environment management.
 *
 * STATE MODEL
 *
 * One `State` object and one `render()`. There is no fine-grained reactivity because
 * there is exactly one view: the cost of re-rendering the whole page on any change is
 * a few hundred nodes, and the benefit is that "what the screen shows" is a pure
 * function of "what the browser said", with no chance of a stale fragment.
 *
 * The ONE thing that survives a re-render is the text the user has typed but not saved
 * (`draft`) plus which section has focus. Without that, every background notification
 * from another window would erase work in progress -- a real risk here, because the
 * toolbar switcher writes to the same store.
 */

import {
  ParraApi,
  EnvironmentInfo,
  EnvironmentList,
  HostMappingFormat,
  WriteResult,
  durabilityOf,
  describeOutcome,
} from './parra_api.js';
/**
 * Stylesheets in a build_webui() target are turned into a MODULE that exports getCss(),
 * not into a stylesheet the browser can link to. `import './parra.css.js'` on its own
 * therefore does nothing visible: the module loads, exports a function, and no rule is
 * ever applied -- which is why the first successful render of this page came up with
 * completely unstyled markup.
 *
 * The CSS is injected below as a <style> element built from this string. A <style>
 * element's TEXT is not subject to script-src, so this does not hit the CSP that blocked
 * the inline <script> in parra.html.
 */
import {getCss} from './parra.css.js';

/**
 * Unwraps the CSSResultGroup that build_webui()'s CSS wrapper returns into plain text.
 *
 * The generated getCss() yields a Lit `CSSResultGroup`, not a string: depending on the
 * wrapper version it is a CSSResult (`.cssText`), or an array of them, or a
 * CSSStyleSheet. Taking the text explicitly keeps this page framework-free -- it uses no
 * Lit components, so it has no shadow root to adopt a stylesheet into.
 */
function cssTextOf(group: unknown): string {
  if (typeof group === 'string') {
    return group;
  }
  if (Array.isArray(group)) {
    return group.map(cssTextOf).join('\n');
  }
  const withText = group as {cssText?: string; styleSheet?: CSSStyleSheet};
  if (typeof withText?.cssText === 'string') {
    return withText.cssText;
  }
  if (withText?.styleSheet) {
    return Array.from(withText.styleSheet.cssRules)
      .map((rule) => rule.cssText)
      .join('\n');
  }
  return '';
}
import {
  Banner,
  ENVIRONMENT_PALETTE,
  HOST_MAPPING_EXAMPLES,
  clear,
  el,
  environmentDot,
  environmentMeta,
  labeledField,
  renderBanner,
} from './parra_elements.js';
import {parseHostMappings} from './host_mapping.js';
import type {HostMappingDiagnostic} from './host_mapping.js';

// ------------------------------------------------------------------------ state

interface Draft {
  name: string;
  color: string;
  hostMappings: string;
  hostMappingFormat: HostMappingFormat;
}

interface State {
  externalLinkPolicy: number;
  externalLinkEnvironment: string;
  externalLinkError: string | null;
  loaded: boolean;
  loadError: string | null;
  list: EnvironmentList | null;
  selectedId: string;

  /** Unsaved edits for the selected environment. Null when there is nothing to edit. */
  draft: Draft | null;

  /** A banner describing the result of the last action. */
  banner: Banner | null;

  /** Client-side validation for the host-mapping field, cleared on a successful save. */
  hostMappingError: string | null;
  hostMappingDiagnostics: HostMappingDiagnostic[];
  /** Errors reported by the store, keyed by field, so they land next to the control. */
  nameError: string | null;

  /** True while a round trip is in flight; disables the action buttons. */
  busy: boolean;

  /**
   * Which card's save button was pressed.
   *
   * The page has two independent saves (基本信息 / 网络映射) that share one draft and
   * one write path. Without this, 保存 also wrote the host mappings -- so renaming an
   * environment would change its network routing with no warning, since the
   * "applies to new tabs only" message belongs to the mapping save.
   */
  pendingSaveFields: 'basic' | 'mappings' | 'all' | null;

  /** The environment pending delete confirmation, or null. */
  confirmingDelete: string | null;
}

const PERSONAL_ID = 'personal';

function isDraftDirty(env: EnvironmentInfo, draft: Draft): boolean {
  return (
    draft.name !== env.name ||
    draft.color !== env.color ||
    draft.hostMappings !== env.hostMappings ||
    draft.hostMappingFormat !== env.hostMappingFormat
  );
}

// ---------------------------------------------------------------------- component

class ParraApp extends HTMLElement {
  /**
   * The mojo client. Read-only public rather than private for one reason: the
   * verification scripts need to drive the same store writes the UI performs, and doing
   * that through this object exercises the real browser path instead of a second
   * implementation of the calls. Reaching it requires an already-trusted chrome://parra
   * document (the only place this element exists), so it exposes nothing that the page
   * could not already do.
   */
  readonly api = new ParraApi();

  private state: State = {
    externalLinkPolicy: 0,
    externalLinkEnvironment: PERSONAL_ID,
    externalLinkError: null,
    loaded: false,
    loadError: null,
    list: null,
    selectedId: PERSONAL_ID,
    draft: null,
    banner: null,
    hostMappingError: null,
    hostMappingDiagnostics: [],
    nameError: null,
    busy: false,
    confirmingDelete: null,
    pendingSaveFields: null,
  };

  // -------------------------------------------------------------------- lifecycle

  connectedCallback() {
    this.injectStyles();
    this.render();
    this.armBootWatchdog();
    void this.start();
  }

  /**
   * Applies parra.css to the document.
   *
   * Guarded by an id so a re-connect (the element being moved in the DOM) does not stack
   * duplicate stylesheets, and appended to <head> rather than to the element itself so
   * the rules also cover html/body, which the layout needs for the full-height shell.
   */
  private injectStyles(): void {
    const id = 'parra-stylesheet';
    if (document.getElementById(id)) {
      return;
    }
    const style = document.createElement('style');
    style.id = id;
    style.textContent = cssTextOf(getCss());
    document.head.appendChild(style);
  }

  /**
   * Reveals the boot-error panel in parra.html if the page never becomes functional.
   *
   * THIS IS THE ONLY PLACE IT CAN LIVE. An inline <script> in parra.html cannot do it:
   * WebUI pages are served under `script-src chrome://resources chrome://webui-test
   * 'self'`, which blocks inline execution, so the watchdog was silently dead and a
   * failed module import left a blank page with no explanation.
   *
   * 3 seconds is generous for a local module graph and short enough that a user staring
   * at a blank page does not conclude the page is broken for another reason.
   */
  private armBootWatchdog(): void {
    window.setTimeout(() => {
      const booted = (window as unknown as {__parraBooted?: boolean}).__parraBooted;
      if (booted) {
        return;
      }
      const panel = document.getElementById('boot-error');
      if (!panel || !panel.hidden) {
        return;
      }
      panel.hidden = false;
      const detail = document.getElementById('boot-error-detail');
      const reason =
        (window as unknown as {__parraBootError?: string}).__parraBootError ??
        '前端模块没有在预期时间内完成初始化。';
      if (detail) {
        detail.textContent = reason;
      }
    }, 3000);
  }

  private async start(): Promise<void> {
    try {
      this.api.connect();
      this.api.onEnvironmentsChanged(() => {
        void this.reload();
      });
      await this.reload();
      const routing = await this.api.getExternalLinkPolicy();
      this.state.externalLinkPolicy = routing.policy;
      this.state.externalLinkEnvironment = routing.environmentId;
      this.render();
      // Tells the boot-error timer in parra.html that the module ran. Set only after a
      // successful first load, so a failure inside the Mojo handshake still surfaces as
      // the visible error panel rather than a blank page.
      (window as unknown as {__parraBooted?: boolean}).__parraBooted = true;
    } catch (error) {
      const message = error instanceof Error ? error.message : String(error);
      (window as unknown as {__parraBootError?: string}).__parraBootError = message;
      this.state.loaded = true;
      this.state.loadError = message;
      this.render();
    }
  }

  /** Re-reads everything. Preserves the selection and any unsaved draft. */
  private async reload(): Promise<void> {
    try {
      const list = await this.api.getEnvironments();
      this.state.list = list;
      this.state.loadError = null;

      // Keep the selection if it still exists; otherwise fall back to Personal. Doing
      // this here (rather than in the click handler) means a deleted-elsewhere
      // environment cannot leave the detail pane pointing at nothing.
      if (!list.environments.some((env) => env.id === this.state.selectedId)) {
        this.state.selectedId = PERSONAL_ID;
        this.state.draft = null;
      }

      // A draft is only meaningful while its environment still exists AND the user has
      // not saved. Rebuilding it from the fresh definition would discard typing on
      // every unrelated notification, so it is left alone.
      const selected = this.selectedEnvironment();
      if (!selected) {
        this.state.draft = null;
      }
    } catch (error) {
      this.state.loadError = error instanceof Error ? error.message : String(error);
    } finally {
      this.state.loaded = true;
      this.render();
    }
  }

  // ------------------------------------------------------------------- selectors

  private environments(): EnvironmentInfo[] {
    return this.state.list?.environments ?? [];
  }

  private selectedEnvironment(): EnvironmentInfo | null {
    return (
      this.environments().find((env) => env.id === this.state.selectedId) ?? null
    );
  }

  private get readOnly(): boolean {
    return this.state.list?.readOnly ?? false;
  }

  // --------------------------------------------------------------------- actions

  private select(id: string): void {
    if (this.state.selectedId === id) {
      return;
    }
    this.state.selectedId = id;
    // Selecting a different environment abandons the draft. Keeping it would apply the
    // previous environment's typing to the new one on the next save.
    this.state.draft = null;
    this.state.hostMappingError = null;
    this.state.hostMappingDiagnostics = [];
    this.state.nameError = null;
    this.state.banner = null;
    this.state.confirmingDelete = null;
    this.render();
  }

  private editDraft(mutate: (draft: Draft) => void): void {
    const env = this.selectedEnvironment();
    if (!env) {
      return;
    }
    if (!this.state.draft) {
      this.state.draft = {
        name: env.name,
        color: env.color,
        hostMappings: env.hostMappings,
        hostMappingFormat: env.hostMappingFormat,
      };
    }
    mutate(this.state.draft);
    // Re-rendering on every keystroke would destroy the caret. The caller therefore
    // updates the draft and re-renders only when the STRUCTURE changes (an error
    // appearing, a button enabling); typing just updates the value.
    this.render();
  }

  private async save(): Promise<void> {
    const env = this.selectedEnvironment();
    const draft = this.state.draft;
    if (!env || !draft || this.state.busy) {
      return;
    }

    // A SAVE CAN BE A NO-OP, AND THAT MUST STILL CLEAR STALE FEEDBACK.
    //
    // Reaching this method with nothing to write happens right after a rejected edit is
    // reverted: the draft matches the stored value again, `dirty` is false, and the
    // button is disabled -- so this path is only reachable via a programmatic call. When
    // it is, the previous failure banner must go: leaving "保存失败" on screen after the
    // user restored a valid value makes the page assert a problem that no longer exists,
    // and there is no longer any way for them to dismiss it.
    const fields = this.state.pendingSaveFields ?? 'all';
    this.state.pendingSaveFields = null;

    const basicRequested = fields === 'all' || fields === 'basic';
    const mappingsRequested = fields === 'all' || fields === 'mappings';

    const nameChanged = basicRequested && draft.name !== env.name;
    const colorChanged = basicRequested && draft.color !== env.color;
    const mappingsChanged = mappingsRequested &&
      (draft.hostMappings !== env.hostMappings ||
       draft.hostMappingFormat !== env.hostMappingFormat);

    if (!nameChanged && !colorChanged && !mappingsChanged) {
      this.state.nameError = null;
      this.state.hostMappingError = null;
      this.state.hostMappingDiagnostics = [];
      this.state.banner = {
        kind: 'info',
        title: '没有需要保存的改动',
        body: '当前值与已保存的值一致，因此没有发出写入。',
      };
      this.render();
      return;
    }

    this.state.busy = true;
    this.state.nameError = null;
    this.state.hostMappingError = null;
    this.state.banner = null;
    this.render();

    // ARE THERE TWO INDEPENDENT SAVES ON THIS PAGE?
    //
    // Yes: 保存 (基本信息) and 保存映射 (网络映射) are separate buttons in separate
    // cards, and each must save only its own card's fields. They used to share one code
    // path, which meant 保存 silently wrote the host mappings too -- a user editing a
    // name would change network routing without seeing the "new tabs only" warning that
    // belongs to that change. `fields` records which button was pressed.
    if (nameChanged || colorChanged) {
      const result = await this.api.updateEnvironment(env.id, {
        ...(nameChanged ? {name: draft.name} : {}),
        ...(colorChanged ? {color: draft.color} : {}),
      });
      if (!this.handleWriteFailure(result, 'name')) {
        this.state.busy = false;
        this.render();
        return;
      }
      // A successful write clears the field-level error. Without this, a rejected name
      // left its red message (and the red input border) on screen after the user fixed
      // it and saved again, so the page kept asserting a problem that no longer existed.
      this.state.nameError = null;
    }

    if (mappingsChanged) {
      const result = await this.api.setHostMappings(
          env.id, draft.hostMappings, draft.hostMappingFormat);
      if (!this.handleWriteFailure(result, 'hostMapping')) {
        this.state.busy = false;
        this.render();
        return;
      }
      this.state.hostMappingError = null;
      switch (result.hostMappingApplyStatus) {
        case 'savedPending':
          this.state.banner = {
            kind: 'warning',
            title: '配置已保存，等待应用',
            body: result.error || '当前没有可更新的 NetworkContext；新建或重启环境后会装入此 revision。',
          };
          break;
        case 'policyBlocked':
          this.state.banner = {
            kind: 'warning',
            title: '代理策略阻止 Host Override',
            body: result.routingPolicyReason || result.error || 'DIRECT 被策略拒绝；匹配请求不会回退到代理。',
          };
          break;
        case 'unknown':
          this.state.banner = {
            kind: 'warning',
            title: '代理策略状态未知',
            body: result.routingPolicyReason || '映射规则已应用到 NetworkContext，但匹配请求会 fail-closed，直至策略来源确定。',
          };
          break;
        case 'failed':
          this.state.banner = {
            kind: 'warning',
            title: '配置已保存，但未应用到 NetworkContext',
            body: result.error,
          };
          break;
        case 'appliedToNetworkContext':
        case 'notRequested':
          this.state.banner = {
            kind: 'success',
            title: '规则已应用到 NetworkContext',
            body: `已应用 revision ${result.appliedHostMappingRevision.toString()}（context ${result.networkContextGeneration.toString()}）。这不代表实际 TCP/TLS/HTTP 连接已验证；刷新页面以发起新请求。`,
          };
          break;
      }
    } else if (nameChanged || colorChanged) {
      this.state.banner = {
        kind: 'success',
        title: this.durabilitySentence(
          '已保存',
          this.state.list?.persistStatus ?? 'unchanged',
        ),
      };
    }

    this.state.busy = false;
    await this.reload();
  }

  /**
   * Turns a failed write into on-screen feedback. Returns false when the write failed,
   * so the caller can stop the sequence.
   *
   * The field that failed decides WHERE the message goes: a rejected name must appear
   * next to the name input, not only in a banner, or the user has to guess which of
   * three fields to change.
   */
  private handleWriteFailure(
    result: WriteResult,
    field: 'name' | 'hostMapping',
  ): boolean {
    const message = describeOutcome(result.outcome);
    if (message === null) {
      // Success, but the durability still needs saying: "in memory only" is not saved.
      const durability = durabilityOf(result.persistStatus);
      if (durability === 'memory') {
        this.state.banner = {
          kind: 'warning',
          title: '已写入内存，但还没有落盘',
          body:
            '配置已生效，但浏览器还没来得及把改动写入磁盘。此时如果崩溃或强制结束进程，这次改动会丢失。',
        };
      } else if (durability === 'failed') {
        this.state.banner = {
          kind: 'error',
          title: '改动没有被保存',
          body: '写入磁盘失败。改动可能仍在内存中，但重启后会丢失。',
        };
      }
      return true;
    }

    const detail = result.error ? `${message}（${result.error}）` : message;
    if (field === 'name') {
      this.state.nameError = detail;
    } else {
      this.state.hostMappingError = detail;
      const lineError = /line\s+(\d+):\s*(.*)$/i.exec(result.error);
      this.state.hostMappingDiagnostics = [{
        line: lineError ? Number(lineError[1]) : 1,
        severity: 'error',
        message: lineError?.[2] ?? detail,
      }];
    }
    this.state.banner = {kind: 'error', title: '保存失败', body: detail};
    return false;
  }

  /** "已保存（已写入磁盘）" style suffix, so the sentence never over-promises. */
  private durabilitySentence(base: string, status: string): string {
    switch (durabilityOf(status as never)) {
      case 'disk':
        return base;
      case 'memory':
        return `${base}（仅在内存中，尚未落盘）`;
      case 'failed':
        return `${base}（但写入磁盘失败）`;
    }
  }

  private async toggleEnabled(env: EnvironmentInfo): Promise<void> {
    if (this.state.busy) {
      return;
    }
    this.state.busy = true;
    this.render();

    const result = await this.api.setEnabled(env.id, !env.enabled);
    const message = describeOutcome(result.outcome);
    this.state.busy = false;

    if (message !== null) {
      this.state.banner = {kind: 'error', title: '无法修改启用状态', body: message};
      this.render();
      return;
    }

    this.state.banner = {
      kind: 'info',
      title: env.enabled ? `已停用「${env.name}」` : `已启用「${env.name}」`,
      body: env.enabled
        ? '停用只影响它能否被选择，不会改动它的存储分区、网络映射或已打开的标签页。'
        : '现在可以从环境切换器中选中它了。',
    };
    await this.reload();
  }

  private requestDelete(env: EnvironmentInfo): void {
    this.state.confirmingDelete = env.id;
    this.render();
  }

  private cancelDelete(): void {
    this.state.confirmingDelete = null;
    this.render();
  }

  private async confirmDelete(env: EnvironmentInfo): Promise<void> {
    this.state.confirmingDelete = null;
    this.state.busy = true;
    this.render();

    const result = await this.api.deleteEnvironment(env.id);
    const message = describeOutcome(result.outcome);
    this.state.busy = false;

    if (message !== null) {
      this.state.banner = {kind: 'error', title: '无法删除', body: message};
      this.render();
      return;
    }

    this.state.banner = {
      kind: 'info',
      title: `已删除「${env.name}」`,
      body:
        '只删除了环境定义。它的 Cookie、缓存、localStorage 等站点数据仍然保留在磁盘上，不会被自动清理。',
    };
    this.state.selectedId = PERSONAL_ID;
    this.state.draft = null;
    await this.reload();
  }

  private async createEnvironment(): Promise<void> {
    if (this.state.busy) {
      return;
    }
    // Create with a name the user can immediately change, rather than an empty record:
    // the store requires a non-empty name, and a blank row in the list would look like
    // a rendering bug.
    const existing = new Set(this.environments().map((env) => env.id));
    let base = 'new-environment';
    let id = base;
    let index = 2;
    while (existing.has(id)) {
      id = `${base}-${index++}`;
    }

    this.state.busy = true;
    this.render();

    const result = await this.api.createEnvironment(
      id, '新环境', '', '', '', 'standardHosts');
    this.state.busy = false;

    const message = describeOutcome(result.outcome);
    if (message !== null) {
      this.state.banner = {kind: 'error', title: '无法新建环境', body: message};
      this.render();
      return;
    }

    if (result.environment) {
      this.state.selectedId = result.environment.id;
      this.state.draft = null;
    }
    this.state.banner = {
      kind: 'success',
      title: '已新建环境',
      body: '给它起个名字，并在需要时配置主机映射。',
    };
    await this.reload();
  }

  // ---------------------------------------------------------------------- render

  private render(): void {
    // Remember where the caret was so re-rendering does not steal focus mid-typing.
    // Without this, the "reload on external change" path would make typing impossible:
    // every keystroke rebuilds the inputs, and the browser would put focus on <body>.
    const active = document.activeElement as HTMLInputElement | HTMLTextAreaElement | null;
    const restoreId = active?.id ?? null;
    const selectionStart = active?.selectionStart ?? null;

    clear(this);

    if (!this.state.loaded) {
      this.appendChild(el('div', {class: 'parra-loading', text: '正在读取环境…'}));
      return;
    }

    if (this.state.loadError) {
      this.appendChild(
        renderBanner({
          kind: 'error',
          title: '无法读取环境配置',
          body: this.state.loadError,
        }),
      );
      this.appendChild(
        el('p', {
          class: 'parra-card-hint',
          text: '页面没有拿到环境数据，因此不显示列表。这不是"没有环境"，而是读取失败。',
        }),
      );
      return;
    }

    const shell = el('div', {class: 'parra-shell'});
    shell.appendChild(this.renderSidebar());
    shell.appendChild(this.renderMain());
    this.appendChild(shell);

    if (this.state.confirmingDelete) {
      const env = this.environments().find(
        (candidate) => candidate.id === this.state.confirmingDelete,
      );
      if (env) {
        this.appendChild(this.renderDeleteDialog(env));
      }
    }

    if (restoreId) {
      const restored = this.querySelector<HTMLInputElement>(`#${CSS.escape(restoreId)}`);
      if (restored) {
        restored.focus();
        if (
          selectionStart !== null &&
          'setSelectionRange' in restored &&
          typeof restored.value === 'string'
        ) {
          const position = Math.min(selectionStart, restored.value.length);
          restored.setSelectionRange(position, position);
        }
      }
    }
  }

  private renderSidebar(): HTMLElement {
    const sidebar = el('aside', {class: 'parra-sidebar'});

    // The header is a title and nothing else. It used to carry a subtitle explaining
    // what an environment is, sitting permanently above the list -- a paragraph the user
    // reads once and then scrolls past forever, while the thing they came for (the
    // list) got pushed down. The explanation now lives in the detail pane, where it is
    // shown for the environment the user is looking at.
    const header = el('div', {class: 'parra-sidebar-header'});
    header.appendChild(el('h1', {class: 'parra-sidebar-title', text: '环境'}));
    sidebar.appendChild(header);

    const list = el('div', {
      class: 'parra-env-list',
      attrs: {role: 'listbox', 'aria-label': '环境列表'},
    });

    const environments = this.environments();

    for (const env of environments) {
      const selected = env.id === this.state.selectedId;
      // A real <button role="option"> rather than a div: it is reachable by Tab,
      // activated by Enter/Space, and announced as a list item. A div with a click
      // handler would look identical and be unusable without a mouse.
      const row = el('button', {
        class: 'parra-env-row',
        attrs: {
          type: 'button',
          role: 'option',
          'aria-selected': selected ? 'true' : 'false',
          ...(env.enabled ? {} : {'data-disabled': 'true'}),
        },
      });

      row.appendChild(environmentDot(env));

      const text = el('div', {class: 'parra-env-row-text'});
      text.appendChild(el('span', {class: 'parra-env-name', text: env.name}));
      text.appendChild(
        el('span', {class: 'parra-env-meta', text: environmentMeta(env)}),
      );
      row.appendChild(text);

      // Roving tabindex: only the selected row is in the Tab order, so Tab moves past
      // the list instead of stopping on every environment, and arrow keys move within
      // it. That is the pattern a listbox is expected to follow; with every row
      // tabbable, a profile with ten environments would put ten tab stops between the
      // header and the detail pane.
      row.tabIndex = selected ? 0 : -1;

      row.addEventListener('click', () => this.select(env.id));
      row.addEventListener('keydown', (event: KeyboardEvent) => {
        // Up/Down move the selection, Home/End jump to the ends. Handled here rather
        // than on the container so the handler knows which row the key was pressed on.
        if (event.key !== 'ArrowDown' && event.key !== 'ArrowUp' &&
            event.key !== 'Home' && event.key !== 'End') {
          return;
        }
        event.preventDefault();
        const index = environments.findIndex((e) => e.id === env.id);
        let next = index;
        if (event.key === 'ArrowDown') {
          next = Math.min(index + 1, environments.length - 1);
        } else if (event.key === 'ArrowUp') {
          next = Math.max(index - 1, 0);
        } else if (event.key === 'Home') {
          next = 0;
        } else {
          next = environments.length - 1;
        }
        if (next === index) {
          return;
        }
        // `noUncheckedIndexedAccess` is on for this target, so the lookup is typed as
        // possibly undefined even though `next` was clamped to a valid index. Checking it
        // rather than asserting keeps the handler safe if `environments` were ever to
        // change between the closure being built and the key being pressed (a store push
        // can re-render the list).
        const nextEnv = environments[next];
        if (!nextEnv) {
          return;
        }
        this.select(nextEnv.id);
        // Focus follows the selection, or the arrow keys would move the detail pane
        // while leaving the keyboard back on the old row.
        queueMicrotask(() => {
          const rows = this.querySelectorAll<HTMLElement>('.parra-env-row');
          rows[next]?.focus();
        });
      });

      list.appendChild(row);
    }

    sidebar.appendChild(list);

    // "New environment" belongs with the list it adds to. It used to sit in the detail
    // pane's header, which meant creating an environment was an action on the thing you
    // were currently looking at rather than on the collection.
    const footer = el('div', {class: 'parra-sidebar-footer'});
    const createButton = el('button', {
      class: 'parra-btn parra-btn--primary parra-btn--block',
      text: '新建环境',
      attrs: {type: 'button'},
    });
    createButton.disabled = this.readOnly || this.state.busy;
    createButton.addEventListener('click', () => {
      void this.createEnvironment();
    });
    footer.appendChild(createButton);
    sidebar.appendChild(footer);

    return sidebar;
  }

  private renderMain(): HTMLElement {
    const main = el('main', {class: 'parra-main'});
    const inner = el('div', {class: 'parra-main-inner'});
    main.appendChild(inner);

    const env = this.selectedEnvironment();

    // The page title is the SELECTED ENVIRONMENT'S NAME.
    //
    // It used to say "环境", which repeated the sidebar's own title one column over and
    // told the user nothing: on a page whose whole job is "which environment am I
    // editing", the answer should be the heading. The static explanation that used to
    // live under it is gone for the same reason -- it described the concept of an
    // environment every time, instead of describing this one.
    const header = el('div', {class: 'parra-page-header'});
    const titles = el('div', {class: 'parra-page-titles'});
    titles.appendChild(el('h2', {class: 'parra-page-title', text: env ? env.name : '环境'}));
    if (env) {
      titles.appendChild(this.renderStatusLine(env));
    }
    header.appendChild(titles);
    inner.appendChild(header);

    // Persistent global state, shown before anything the user can act on.
    if (this.state.list?.readOnly) {
      inner.appendChild(
        renderBanner({
          kind: 'warning',
          title: '环境配置处于只读状态',
          body:
            '保存的配置由更新版本的浏览器写入，当前版本不会覆盖它。可以查看，但无法新建、修改或删除。',
        }),
      );
    }

    if (this.state.list?.loadDiagnostics) {
      inner.appendChild(
        renderBanner({
          kind: 'warning',
          title: '读取配置时有问题',
          body: this.state.list.loadDiagnostics,
        }),
      );
    }

    if (this.state.banner) {
      inner.appendChild(renderBanner(this.state.banner));
    }

    if (!env) {
      const empty = el('div', {class: 'parra-empty'});
      empty.appendChild(el('p', {class: 'parra-empty-title', text: '没有选中的环境'}));
      empty.appendChild(
        el('p', {
          class: 'parra-empty-body',
          text: '从左侧列表中选择一个环境，或新建一个。',
        }),
      );
      inner.appendChild(empty);
      return main;
    }

    // Fixed order, most-used first: the two things a user comes here to change, then
    // reference information, then the destructive action last.
    inner.appendChild(this.renderExternalLinkPolicy());
    inner.appendChild(this.renderBasics(env));
    inner.appendChild(this.renderNetwork(env));
    inner.appendChild(this.renderInfo(env));
    if (!env.isPersonal) {
      inner.appendChild(this.renderDanger(env));
    }

    return main;
  }

  private renderExternalLinkPolicy(): HTMLElement {
    const card = el('section', {class: 'parra-card parra-card--compact'});
    card.appendChild(el('h3', {class: 'parra-card-title', text: '外部链接'}));
    card.appendChild(el('p', {class: 'parra-card-hint',
      text: '其他应用打开网页时，决定使用哪个浏览环境。普通网页导航不受影响。'}));
    const policy = el('select', {class: 'parra-route-select',
      attrs: {'aria-label': '外部链接打开方式'}});
    for (const [value, label] of ([
      ['0', '每次询问（默认）'], ['1', '当前活动窗口环境'],
      ['2', '固定指定环境'],
    ] as const)) {
      const option = el('option', {text: label, attrs: {value}});
      policy.appendChild(option);
    }
    policy.value = String(this.state.externalLinkPolicy);
    card.appendChild(policy);

    const fixed = el('select', {class: 'parra-route-select',
      attrs: {'aria-label': '固定打开环境'}});
    for (const environment of this.environments()) {
      const option = el('option', {text: environment.name,
        attrs: {value: environment.id}});
      option.disabled = !environment.enabled;
      fixed.appendChild(option);
    }
    fixed.value = this.state.externalLinkEnvironment;
    fixed.hidden = policy.value !== '2';
    policy.addEventListener('change', () => { fixed.hidden = policy.value !== '2'; });
    card.appendChild(fixed);

    const save = el('button', {class: 'parra-btn parra-btn--primary',
      text: '保存外部链接设置', attrs: {type: 'button'}});
    save.addEventListener('click', async () => {
      save.disabled = true;
      const nextPolicy = Number(policy.value);
      const nextId = nextPolicy === 2 ? fixed.value : PERSONAL_ID;
      try {
        const result = await this.api.setExternalLinkPolicy(nextPolicy, nextId);
        this.state.externalLinkError = result.success ? null : result.error;
        if (result.success) {
          this.state.externalLinkPolicy = nextPolicy;
          this.state.externalLinkEnvironment = nextId;
          this.state.banner = {kind: 'success', title: '外部链接设置已保存',
            body: '下一次从其他应用打开网址时生效。'};
        }
      } catch (error) {
        this.state.externalLinkError = String(error);
      }
      this.render();
    });
    card.appendChild(save);
    if (this.state.externalLinkError) {
      card.appendChild(el('p', {class: 'parra-field-error',
        text: this.state.externalLinkError}));
    }
    return card;
  }

  /** One-line summary under the page title: environment type, enabled state and tabs. */
  private renderStatusLine(env: EnvironmentInfo): HTMLElement {
    const line = el('p', {class: 'parra-status-line'});

    const dot = el('span', {class: 'parra-status-dot'});
    dot.style.backgroundColor = env.color || 'var(--parra-text-tertiary)';
    line.appendChild(dot);

    const parts: string[] = [];
    parts.push(env.isPersonal ? '默认环境' : '自定义环境');
    parts.push(env.enabled ? '已启用' : '已停用');
    parts.push(env.tabCount === 0 ? '无标签页' : `${env.tabCount} 个标签页`);
    line.appendChild(el('span', {class: 'parra-status-text', text: parts.join(' · ')}));

    return line;
  }

  // ------------------------------------------------------------------ 基本信息

  private renderBasics(env: EnvironmentInfo): HTMLElement {
    const card = el('section', {class: 'parra-card'});
    card.appendChild(el('h3', {class: 'parra-card-title', text: '基本信息'}));

    const draft = this.state.draft;
    const nameValue = draft ? draft.name : env.name;
    const colorValue = draft ? draft.color : env.color;
    const disabled = this.readOnly || env.isPersonal;

    // NAME
    const nameInput = el('input', {
      class: this.state.nameError
        ? 'parra-input parra-input--invalid'
        : 'parra-input',
      attrs: {
        id: 'parra-name',
        type: 'text',
        value: nameValue,
        maxlength: '64',
        ...(disabled ? {disabled: ''} : {}),
      },
    });
    nameInput.value = nameValue;
    nameInput.addEventListener('input', () => {
      this.state.nameError = null;
      this.editDraft((current) => {
        current.name = nameInput.value;
      });
    });

    card.appendChild(
      labeledField(
        {
          label: '名称',
          hint: env.isPersonal
            ? '个人环境是保留环境，名称不能修改。'
            : '显示在环境切换器和这个列表里。可以随时修改，不影响已保存的站点数据。',
          error: this.state.nameError ?? undefined,
        },
        nameInput,
        'parra-name',
      ),
    );

    // COLOUR
    card.appendChild(
      labeledField(
        {
          label: '颜色',
          hint: '仅用于在列表和切换器中区分环境，不影响任何数据或网络行为。',
        },
        this.renderColourPicker(colorValue, disabled),
        'parra-colour',
      ),
    );

    // ENABLE / DISABLE
    const switchRow = el('div', {class: 'parra-switch-row'});
    const switchText = el('div', {class: 'parra-switch-text'});
    switchText.appendChild(
      el('p', {class: 'parra-switch-label', text: '启用此环境'}),
    );
    switchText.appendChild(
      el('p', {
        class: 'parra-switch-hint',
        text: env.isPersonal
          ? '个人环境始终可用，不能停用。'
          : '停用后它不会出现在环境切换器中，但已有的标签页和存储数据都保持不变。这是一个选择层的开关。',
      }),
    );
    switchRow.appendChild(switchText);

    const toggle = el('button', {
      class: env.enabled ? 'parra-btn parra-btn--quiet' : 'parra-btn',
      text: env.enabled ? '停用' : '启用',
      attrs: {type: 'button'},
    });
    toggle.disabled = this.readOnly || env.isPersonal || this.state.busy;
    toggle.addEventListener('click', () => {
      void this.toggleEnabled(env);
    });
    switchRow.appendChild(toggle);
    card.appendChild(switchRow);

    // SAVE
    const dirty = draft !== null && isDraftDirty(env, draft);
    const actions = el('div', {class: 'parra-actions parra-actions--end'});

    if (dirty) {
      const discard = el('button', {
        class: 'parra-btn parra-btn--quiet',
        text: '放弃修改',
        attrs: {type: 'button'},
      });
      discard.disabled = this.state.busy;
      discard.addEventListener('click', () => {
        this.state.draft = null;
        this.state.nameError = null;
        this.state.hostMappingError = null;
        this.state.banner = null;
        this.render();
      });
      actions.appendChild(discard);
    }

    const save = el('button', {
      class: 'parra-btn parra-btn--primary',
      text: this.state.busy ? '保存中…' : '保存',
      attrs: {type: 'button'},
    });
    // Disabled when nothing changed, so the button's state answers "is there anything
    // to save" instead of inviting a no-op write that would still report success.
    save.disabled = !dirty || this.state.busy || this.readOnly || env.isPersonal;
    save.addEventListener('click', () => {
      // Scope: this button belongs to 基本信息, so it must not write host mappings.
      this.state.pendingSaveFields = 'basic';
      void this.save();
    });
    actions.appendChild(save);
    card.appendChild(actions);

    return card;
  }

  private renderColourPicker(current: string, disabled: boolean): HTMLElement {
    const grid = el('div', {class: 'parra-colour-grid', attrs: {id: 'parra-colour'}});

    // "No colour" first: choosing it is a real option, and putting it in the same shape
    // as the colours keeps the row scannable.
    const none = el('button', {
      class: 'parra-colour-swatch parra-colour-swatch--none',
      attrs: {
        type: 'button',
        title: '不设置颜色',
        'aria-label': '不设置颜色',
        'aria-pressed': current === '' ? 'true' : 'false',
        ...(disabled ? {disabled: ''} : {}),
      },
    });
    none.addEventListener('click', () => {
      this.state.nameError = null;
      this.editDraft((draft) => {
        draft.color = '';
      });
    });
    grid.appendChild(none);

    for (const colour of ENVIRONMENT_PALETTE) {
      const swatch = el('button', {
        class: 'parra-colour-swatch',
        attrs: {
          type: 'button',
          title: colour,
          'aria-label': `颜色 ${colour}`,
          'aria-pressed': current === colour ? 'true' : 'false',
          ...(disabled ? {disabled: ''} : {}),
        },
      });
      swatch.style.backgroundColor = colour;
      swatch.addEventListener('click', () => {
        this.state.nameError = null;
        this.editDraft((draft) => {
          draft.color = colour;
        });
      });
      grid.appendChild(swatch);
    }

    return grid;
  }

  // ------------------------------------------------------------------ 网络映射

  private renderNetwork(env: EnvironmentInfo): HTMLElement {
    const card = el('section', {class: 'parra-card'});
    card.appendChild(el('h3', {class: 'parra-card-title', text: '网络映射'}));
    card.appendChild(
      el('p', {
        class: 'parra-card-hint',
        text: env.isPersonal
          ? '个人环境的规则作用于默认网络上下文，也就是所有不属于具名环境的标签页。影响范围比其他环境更大。'
          : '只作用于这个环境。同一个域名可以在不同环境中解析到不同地址，因此可以并行登录同一站点的多个账号。',
      }),
    );

    const draft = this.state.draft;
    const value = draft ? draft.hostMappings : env.hostMappings;
    const format = draft ? draft.hostMappingFormat : env.hostMappingFormat;

    const isDefault = value.trim() === '';
    card.appendChild(
      renderBanner({
        kind: 'info',
        title: isDefault ? '当前没有映射规则' : '保存后立即更新映射',
        body: isDefault
          ? '这个环境的请求按系统 DNS 解析，不做任何改写。'
          : '保存后会更新当前环境的网络规则。已打开的页面不会自动重新加载；刷新或新开页面即可使用新规则。',
      }),
    );

    const formatSelect = el('select', {
      class: 'parra-select',
      attrs: {id: 'parra-host-mapping-format', ...(this.readOnly ? {disabled: ''} : {})},
    });
    const formats: Array<{value: HostMappingFormat; label: string}> = [
      {value: 'standardHosts', label: 'Standard Hosts（默认）'},
      {value: 'whistleHost', label: 'Whistle Host（兼容子集）'},
      {value: 'chromiumMap', label: 'Chromium MAP/EXCLUDE（旧格式）'},
    ];
    if (format === 'unsupported') {
      formats.push({value: 'unsupported', label: '未知格式（只读）'});
    }
    for (const item of formats) {
      formatSelect.appendChild(el('option', {
        text: item.label,
        attrs: {value: item.value},
      }));
    }
    formatSelect.value = format;
    card.appendChild(labeledField(
        {label: '输入格式', hint: '格式会随规则一起保存；更改格式不会改写原文。'},
        formatSelect, 'parra-host-mapping-format'));

    const parsed = parseHostMappings(value, format);
    const parsedError = parsed.diagnostics.find((item) => item.severity === 'error');
    const previousServerError = this.state.hostMappingError &&
        this.state.hostMappingDiagnostics.some((item) => item.severity === 'error') &&
        !parsedError ? this.state.hostMappingDiagnostics : [];
    const displayedDiagnostics = parsedError ?
      parsed.diagnostics : previousServerError.length > 0 ?
        previousServerError : parsed.diagnostics;
    const diagnostics = el('ul', {
      class: 'parra-mapping-diagnostics',
      attrs: {id: 'parra-host-mappings-diagnostics', role: 'status', 'aria-live': 'polite'},
    });
    const firstError = displayedDiagnostics.find((item) => item.severity === 'error');
    this.state.hostMappingDiagnostics = displayedDiagnostics;
    this.state.hostMappingError = firstError ?
      previousServerError.length > 0 && !parsedError ? this.state.hostMappingError :
        `第 ${firstError.line} 行：${firstError.message}` : null;

    const textarea = el('textarea', {
      class: firstError ? 'parra-textarea parra-textarea--invalid' : 'parra-textarea',
      attrs: {
        id: 'parra-host-mappings',
        rows: '7',
        spellcheck: 'false',
        placeholder: HOST_MAPPING_EXAMPLES[format === 'unsupported' ? 'chromiumMap' : format],
        'aria-describedby': 'parra-host-mappings-diagnostics',
        ...(firstError ? {'aria-invalid': 'true'} : {}),
        ...(this.readOnly ? {disabled: ''} : {}),
      },
    });
    textarea.value = value;
    let syncMappingActions: (() => void) | null = null;
    let mappingField: HTMLElement | null = null;
    const renderDiagnostics = (items: HostMappingDiagnostic[]) => {
      clear(diagnostics);
      for (const item of items) {
        diagnostics.appendChild(el('li', {
          class: item.severity === 'error' ?
            'parra-mapping-diagnostic parra-mapping-diagnostic--error' :
            'parra-mapping-diagnostic parra-mapping-diagnostic--warning',
          text: `第 ${item.line} 行：${item.message}`,
        }));
      }
      if (items.length === 0) {
        diagnostics.appendChild(el('li', {
          class: 'parra-mapping-diagnostic parra-mapping-diagnostic--ok',
          text: '格式检查通过。',
        }));
      }
    };
    renderDiagnostics(displayedDiagnostics);

    const applyDraftMappings = (nextText: string, nextFormat: HostMappingFormat) => {
      if (!this.state.draft) {
        this.state.draft = {
          name: env.name,
          color: env.color,
          hostMappings: nextText,
          hostMappingFormat: nextFormat,
        };
      } else {
        this.state.draft = {
          ...this.state.draft,
          hostMappings: nextText,
          hostMappingFormat: nextFormat,
        };
      }
      const result = parseHostMappings(nextText, nextFormat);
      this.state.hostMappingDiagnostics = result.diagnostics;
      const error = result.diagnostics.find((item) => item.severity === 'error');
      this.state.hostMappingError = error ?
        `第 ${error.line} 行：${error.message}` : null;
      textarea.classList.toggle('parra-textarea--invalid', error !== undefined);
      if (error) {
        textarea.setAttribute('aria-invalid', 'true');
      } else {
        textarea.removeAttribute('aria-invalid');
      }
      renderDiagnostics(result.diagnostics);
      syncMappingActions?.();
    };

    textarea.addEventListener('input', () => {
      // Keep the caret and focus in the editor while updating its error and actions.
      applyDraftMappings(textarea.value, formatSelect.value as HostMappingFormat);
    });

    formatSelect.addEventListener('change', () => {
      const previousFormat = this.state.draft?.hostMappingFormat ?? env.hostMappingFormat;
      const nextFormat = formatSelect.value as HostMappingFormat;
      const preview = parseHostMappings(textarea.value, nextFormat);
      const errorCount = preview.diagnostics.filter((item) => item.severity === 'error').length;
      const previewText = errorCount > 0 ?
        `${errorCount} 个错误；当前内容不会生成可应用规则。` :
        (preview.compiledRules || '（无映射规则）');
      const confirmed = nextFormat !== 'unsupported' && window.confirm(
          `将保留原文，仅改用新格式解释。\n\n编译预览：\n${previewText}\n\n确认切换？`);
      if (!confirmed) {
        formatSelect.value = previousFormat;
        return;
      }
      applyDraftMappings(textarea.value, nextFormat);
      this.render();
    });

    mappingField =
      labeledField(
        {
          label: '主机映射规则',
          error: undefined,
          hint: undefined,
        },
        textarea,
        'parra-host-mappings',
      );
    card.appendChild(mappingField);
    mappingField.appendChild(diagnostics);

    // The example box: shown as its own element so it can be selected and copied
    // without being confused for the current value.
    const example = el('div', {class: 'parra-field-hint'});
    example.appendChild(el('span', {text: '规则示例：'}));
    const exampleText = HOST_MAPPING_EXAMPLES[
      format === 'unsupported' ? 'chromiumMap' : format];
    example.appendChild(el('code', {class: 'parra-code', text: exampleText}));
    example.appendChild(
      el('span', {
        text: format === 'standardHosts' ?
          '。每行以 IP 开头，后接一个或多个域名；支持 IPv4/IPv6 和 # 注释。' :
          format === 'whistleHost' ?
            '。只支持静态主机到 IP 的子集；路径、过滤器、正则和代理规则明确不支持。' :
            '。继续使用旧版 Chromium MAP/EXCLUDE 语法。留空表示不映射。',
      }),
    );
    card.appendChild(example);

    const dirty = draft !== null &&
      (draft.hostMappings !== env.hostMappings ||
       draft.hostMappingFormat !== env.hostMappingFormat);
    const actions = el('div', {class: 'parra-actions parra-actions--end'});

    const discard = el('button', {
      class: 'parra-btn parra-btn--quiet',
      text: '放弃修改',
      attrs: {type: 'button'},
    });
    discard.disabled = this.state.busy;
    discard.hidden = !dirty;
    discard.addEventListener('click', () => {
      this.state.hostMappingError = null;
      this.state.hostMappingDiagnostics = [];
      this.state.draft = null;
      this.state.banner = null;
      this.render();
    });
    actions.appendChild(discard);

    const save = el('button', {
      class: 'parra-btn parra-btn--primary',
      text: this.state.busy ? '保存中…' : '保存映射',
      attrs: {type: 'button'},
    });
    save.disabled = !dirty || this.state.busy || this.readOnly ||
      this.state.hostMappingError !== null;
    save.addEventListener('click', () => {
      // Scope: this button belongs to 网络映射, so it must not rename the environment.
      this.state.pendingSaveFields = 'mappings';
      void this.save();
    });
    actions.appendChild(save);
    syncMappingActions = () => {
      const current = this.state.draft;
      const changed = current !== null &&
        (current.hostMappings !== env.hostMappings ||
         current.hostMappingFormat !== env.hostMappingFormat);
      discard.hidden = !changed;
      save.disabled = !changed || this.state.busy || this.readOnly ||
        this.state.hostMappingError !== null;
    };
    card.appendChild(actions);

    return card;
  }

  // ------------------------------------------------------------------ 环境信息

  private renderInfo(env: EnvironmentInfo): HTMLElement {
    // A compact reference panel, not a card with a heading, an explanation and four
    // labelled rows. Everything here is read-only and most of it repeats something the
    // page already shows (the name is the title, the state is the status line), so it is
    // presented as a single quiet block at the bottom rather than as a section
    // competing with the two the user actually edits.
    const card = el('section', {class: 'parra-card parra-card--compact'});
    card.appendChild(el('h3', {class: 'parra-card-title', text: '环境详情'}));

    // Plain divs and spans rather than <dl>/<dt>/<dd>.
    //
    // <dt> maps to the ARIA `term` role, and `term` is one of the roles that the ARIA
    // specification forbids from carrying an accessible name (a <dt> derives its name
    // from its contents, so naming it is contradictory). Chromium enforces that rule on
    // WebUI with a DCHECK, and this build has DCHECK_ALWAYS_ON, so the check does not
    // politely log: it aborts the renderer. The rule only runs once the accessibility
    // tree has been activated -- which any screen reader does, and so does the UIA
    // automation the test scripts use -- so the page looked fine in ordinary manual use
    // and then silently died the moment accessibility was switched on. Since the
    // definition-list semantics were never used by anything (the visual layout comes from
    // the grid below, and there is no <dl> relationship a reader could act on), replacing
    // the elements removes the crash without losing meaning.
    const list = el('div', {class: 'parra-deflist'});

    const term = (text: string): HTMLElement => el('span', {class: 'parra-deflist-term', text});
    const value = (node: HTMLElement | string): HTMLElement => {
      const v = el('span', {class: 'parra-deflist-value'});
      if (typeof node === 'string') {
        v.textContent = node;
      } else {
        v.appendChild(node);
      }
      return v;
    };

    list.appendChild(term('环境 ID'));
    list.appendChild(value(el('span', {class: 'parra-mono-value', text: env.id})));

    list.appendChild(term('存储隔离'));
    list.appendChild(value(
        env.isPersonal ?
            '浏览器默认存储分区' :
            '独立存储分区，Cookie / 缓存 / localStorage / IndexedDB 与其他环境互不可见'));

    card.appendChild(list);
    return card;
  }

  // ------------------------------------------------------------------ 危险操作

  private renderDanger(env: EnvironmentInfo): HTMLElement {
    const card = el('section', {class: 'parra-card parra-card--danger'});
    card.appendChild(el('h3', {class: 'parra-card-title', text: '危险操作'}));

    // The two paragraphs that used to sit here permanently are now inside a collapsed
    // <details>. The thing a user must understand is what does NOT happen ("delete" is
    // the wrong mental model: the definition goes away, the data does not), so the text
    // stays -- but keeping it always-open meant the last card on the page was the wordiest
    // one, and it was read far more often by people scrolling past than by people who
    // were actually about to delete something. The summary carries the one-sentence
    // version, so the essential fact is still visible with the section collapsed.
    const details = el('details', {class: 'parra-details'});
    details.appendChild(
      el('summary', {
        class: 'parra-details-summary',
        text: '删除只移除定义，不会清理已保存的站点数据',
      }),
    );
    const warning = el('div', {class: 'parra-card-hint parra-details-body'});
    warning.appendChild(
      el('p', {
        text:
          '删除环境只删除它的定义（名称、颜色、Host Mapping）。它已经写入磁盘的 Cookie、缓存、' +
          'localStorage、IndexedDB 等站点数据不会被自动清理，仍然保留。',
      }),
    );
    warning.appendChild(
      el('p', {
        text:
          '重建同 ID 的环境后，原来那些站点数据会再次被这个环境使用。如果目的是"彻底清空某个环境的数据"，' +
          '删除定义本身做不到这一点。',
      }),
    );
    details.appendChild(warning);
    card.appendChild(details);

    const blocked = env.tabCount > 0;
    // Blocked is a state the user must see WITHOUT opening anything, so it stays outside
    // the <details>: it explains why the button below is disabled.
    if (blocked) {
      card.appendChild(
        renderBanner({
          kind: 'warning',
          title: '当前无法删除',
          body: `这个环境还有 ${env.tabCount} 个标签页处于打开状态。请先关闭它们，否则这些页面会因为所属环境消失而无法解释。`,
        }),
      );
    }

    const actions = el('div', {class: 'parra-actions'});
    const deleteButton = el('button', {
      class: 'parra-btn parra-btn--danger',
      text: '删除这个环境',
      attrs: {type: 'button'},
    });
    deleteButton.disabled = blocked || this.readOnly || this.state.busy;
    deleteButton.addEventListener('click', () => this.requestDelete(env));
    actions.appendChild(deleteButton);
    card.appendChild(actions);

    return card;
  }

  private renderDeleteDialog(env: EnvironmentInfo): HTMLElement {
    const backdrop = el('div', {class: 'parra-dialog-backdrop'});
    const dialog = el('div', {
      class: 'parra-dialog',
      attrs: {
        role: 'dialog',
        'aria-modal': 'true',
        'aria-labelledby': 'parra-delete-title',
      },
    });

    dialog.appendChild(
      el('h3', {
        class: 'parra-dialog-title',
        text: `删除「${env.name}」？`,
        attrs: {id: 'parra-delete-title'},
      }),
    );

    const body = el('div', {class: 'parra-dialog-body'});
    body.appendChild(
      el('p', {
        text: '这个环境的定义会被永久删除，无法撤销。已有标签页中没有属于它的，所以此操作可以继续。',
      }),
    );
    body.appendChild(
      el('p', {
        text:
          '请注意：它已经保存的 Cookie、缓存和站点数据不会被删除。删除定义不会释放磁盘空间，' +
          '也不会让这些数据变成"从未存在过"。',
      }),
    );
    dialog.appendChild(body);

    const actions = el('div', {class: 'parra-dialog-actions'});

    const cancel = el('button', {
      class: 'parra-btn',
      text: '取消',
      attrs: {type: 'button'},
    });
    cancel.addEventListener('click', () => this.cancelDelete());
    actions.appendChild(cancel);

    const confirm = el('button', {
      class: 'parra-btn parra-btn--danger',
      text: '确认删除定义',
      attrs: {type: 'button'},
    });
    confirm.disabled = this.state.busy;
    confirm.addEventListener('click', () => {
      void this.confirmDelete(env);
    });
    actions.appendChild(confirm);

    dialog.appendChild(actions);
    backdrop.appendChild(dialog);

    // Escape closes the dialog: a modal that can only be dismissed with the mouse is a
    // trap for keyboard users.
    backdrop.addEventListener('keydown', (event: KeyboardEvent) => {
      if (event.key === 'Escape') {
        event.preventDefault();
        this.cancelDelete();
      }
    });
    // Focus the cancel button, not the destructive one, so a reflexive Enter is safe.
    queueMicrotask(() => cancel.focus());

    return backdrop;
  }
}

customElements.define('parra-app', ParraApp);
