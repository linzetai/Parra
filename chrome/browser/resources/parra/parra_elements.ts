// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/**
 * Small, pure rendering helpers.
 *
 * WHY PURE FUNCTIONS INSTEAD OF MORE CUSTOM ELEMENTS
 *
 * The page has one view. Splitting it into six custom elements would mean six
 * lifecycles to keep in sync with one piece of state, which is more moving parts than
 * the problem needs. These helpers build DOM from plain data, so the main element keeps
 * a single render path and the interesting logic (validation, dirty state, focus
 * preservation) stays in one place.
 *
 * Every helper returns a real element rather than an HTML string. That matters here
 * because the page renders USER-SUPPLIED values (environment names, host mappings):
 * building them with `innerHTML` would make any future field an injection point, and
 * being unable to make that mistake is worth the slightly longer code.
 */

import {EnvironmentInfo} from './parra_api.js';

// ------------------------------------------------------------------------ elements

interface ElOptions {
  class?: string;
  text?: string;
  attrs?: Record<string, string>;
  children?: (Node | null | undefined)[];
}

/** Creates an element with classes, text, attributes and children in one call. */
export function el<K extends keyof HTMLElementTagNameMap>(
  tag: K,
  options: ElOptions = {},
): HTMLElementTagNameMap[K] {
  const node = document.createElement(tag);
  if (options.class) {
    node.className = options.class;
  }
  // textContent, never innerHTML: every string that reaches this function may come from
  // the user (a name, a host-mapping rule) or from the browser (a diagnostic).
  if (options.text !== undefined) {
    node.textContent = options.text;
  }
  for (const [key, value] of Object.entries(options.attrs ?? {})) {
    node.setAttribute(key, value);
  }
  for (const child of options.children ?? []) {
    if (child) {
      node.appendChild(child);
    }
  }
  return node;
}

/** Removes every child. Cheaper and clearer than re-creating the container. */
export function clear(node: Element): void {
  while (node.firstChild) {
    node.removeChild(node.firstChild);
  }
}

// ------------------------------------------------------------------- colour dots

/**
 * The palette the page offers.
 *
 * A FIXED SET, not a free colour picker. Two reasons:
 *   - The colour's only job is to make a row distinguishable in a short list. A
 *     free picker invites colours that fail contrast against one of the two themes,
 *     and nothing in the product can compensate for that.
 *   - The stored value is a plain "#RRGGBB" string validated by the store, so a fixed
 *     set also means the picker can never produce a value the store rejects.
 */
export const ENVIRONMENT_PALETTE: string[] = [
  '#2f6b4f', // green: the product accent
  '#3f6ea8', // blue
  '#8a5a3f', // clay
  '#7c5c9e', // muted violet
  '#a8544a', // brick
  '#4f7a80', // teal grey
  '#8a7a34', // ochre
  '#5c6670', // slate
];

/**
 * The row marker.
 *
 * `color === ''` is drawn as a hollow ring rather than a grey dot: an unset colour and
 * a deliberately grey colour must not look identical, or the user cannot tell whether
 * they have configured the environment.
 */
export function environmentDot(env: EnvironmentInfo): HTMLElement {
  const dot = el('span', {
    class: env.color ? 'parra-env-dot' : 'parra-env-dot parra-env-dot--none',
  });
  if (env.color) {
    dot.style.backgroundColor = env.color;
  }
  return dot;
}

// ------------------------------------------------------------------- list rows

/** "3 个标签页" / "无标签页" / "已停用". */
export function environmentMeta(env: EnvironmentInfo): string {
  const parts: string[] = [];
  if (env.isPersonal) {
    parts.push('默认环境');
  }
  if (!env.enabled && !env.isPersonal) {
    parts.push('已停用');
  }
  if (env.tabCount === 0) {
    parts.push('无标签页');
  } else {
    parts.push(`${env.tabCount} 个标签页`);
  }
  return parts.join(' · ');
}

// --------------------------------------------------------------------- banners

export type BannerKind = 'info' | 'warning' | 'error' | 'success';

export interface Banner {
  kind: BannerKind;
  title: string;
  body?: string;
}

export function renderBanner(banner: Banner): HTMLElement {
  const node = el('div', {class: `parra-banner parra-banner--${banner.kind}`});
  // role=status for the informational kinds so a screen reader announces a save
  // result without stealing focus (alert would interrupt the user mid-typing).
  node.setAttribute('role', banner.kind === 'error' ? 'alert' : 'status');

  const text = el('div', {class: 'parra-banner-text'});
  text.appendChild(el('p', {class: 'parra-banner-title', text: banner.title}));
  if (banner.body) {
    text.appendChild(el('p', {class: 'parra-banner-body', text: banner.body}));
  }
  node.appendChild(text);
  return node;
}

// ---------------------------------------------------------------------- fields

export interface FieldSpec {
  label: string;
  /** A note under the control, always visible. */
  hint?: string;
  /** A validation problem, shown in place of the hint when present. */
  error?: string;
}

/**
 * Wraps a control in its label + hint/error.
 *
 * The error is attached with `aria-describedby` rather than only coloured, so the
 * problem is reachable without sight and without hovering.
 */
export function labeledField(
  spec: FieldSpec,
  control: HTMLElement,
  controlId: string,
): HTMLElement {
  const wrapper = el('div', {class: 'parra-field'});

  const label = el('label', {
    class: 'parra-label',
    text: spec.label,
    attrs: {for: controlId},
  });
  wrapper.appendChild(label);
  wrapper.appendChild(control);

  if (spec.error) {
    const errorId = `${controlId}-error`;
    control.setAttribute('aria-invalid', 'true');
    control.setAttribute('aria-describedby', errorId);
    wrapper.appendChild(
      el('p', {class: 'parra-field-error', text: spec.error, attrs: {id: errorId}}),
    );
  } else if (spec.hint) {
    const hintId = `${controlId}-hint`;
    control.setAttribute('aria-describedby', hintId);
    wrapper.appendChild(
      el('p', {class: 'parra-field-hint', text: spec.hint, attrs: {id: hintId}}),
    );
  }

  return wrapper;
}

// -------------------------------------------------------------- host mappings

export const HOST_MAPPING_EXAMPLES = {
  standardHosts: '# 一个 IP 可对应多个域名\n203.0.113.10 example.com api.example.com',
  whistleHost: '# Whistle Host 静态映射子集\nexample.com 203.0.113.10\n*.api.example.com 2001:db8::10',
  chromiumMap: '# 旧版 Chromium 规则\nMAP example.com 203.0.113.10\nEXCLUDE localhost',
};
