// Copyright 2026 The Parra Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/**
 * The page's only channel to the browser process.
 *
 * WHY A WRAPPER INSTEAD OF CALLING THE MOJO REMOTE DIRECTLY
 *
 * Three things would otherwise be repeated at every call site, and repeated subtly
 * differently:
 *
 *   1. The enums arrive as NUMBERS, not names. Mojo maps a mojom enum to the generated
 *      constant, but once the value is on the wire the frontend only has integers. A
 *      `switch (result.outcome === StoreOutcome.kOk)` comparison against a number from
 *      a different module instance is a silent false. Converting once, here, means the
 *      UI compares against strings and a mismatch is visible in the code.
 *
 *   2. The same value can mean different things. `StoreOutcome.kOk` on a host-mapping
 *      write means the rule was stored; the separate apply-failure flag tells the UI
 *      whether updating the live NetworkContext failed. Centralising the conversion
 *      keeps partial success from being presented as full success.
 *
 *   3. Every reply carries a persist status, and the three "successful" persist states
 *      are not equally durable. Turning them into one `durability` field is what lets
 *      the UI say the right sentence instead of "saved" for all three.
 */

import {
  PageCallbackRouter,
  PageHandlerFactory,
  PageHandlerFactoryRemote,
  PageHandlerRemote,
} from './parra.mojom-webui.js';

// ----------------------------------------------------------------------------- types

export type PersistStatus = 'unchanged' | 'memoryUpdated' | 'persisted' | 'failed';

export type HostMappingFormat =
  | 'standardHosts'
  | 'whistleHost'
  | 'chromiumMap'
  | 'unsupported';

export type StoreOutcome =
  | 'ok'
  | 'invalidId'
  | 'duplicateId'
  | 'notFound'
  | 'reservedId'
  | 'invalidField'
  | 'invalidHostMapping'
  | 'readOnly'
  | 'persistFailed'
  | 'refusedTabsOpen';

/** How much of a write actually reached the disk. */
export type Durability = 'memory' | 'disk' | 'failed';

export type HostMappingApplyStatus =
  | 'notRequested'
  | 'savedPending'
  | 'appliedToNetworkContext'
  | 'policyBlocked'
  | 'unknown'
  | 'failed';

export interface EnvironmentInfo {
  id: string;
  name: string;
  color: string;
  icon: string;
  hostMappings: string;
  hostMappingFormat: HostMappingFormat;
  enabled: boolean;
  isPersonal: boolean;
  tabCount: number;
}

export interface EnvironmentList {
  environments: EnvironmentInfo[];
  readOnly: boolean;
  persistStatus: PersistStatus;
  loadDiagnostics: string;
}

export interface WriteResult {
  outcome: StoreOutcome;
  error: string;
  persistStatus: PersistStatus;
  /** True when storage succeeded but updating the live NetworkContext failed. */
  hostMappingApplyFailed: boolean;
  hostMappingApplyStatus: HostMappingApplyStatus;
  desiredHostMappingRevision: bigint;
  appliedHostMappingRevision: bigint;
  networkContextGeneration: bigint;
  routingPolicyReason: string;
  environment: EnvironmentInfo | null;
}

// ------------------------------------------------------------------------ enum maps

const PERSIST_STATUS: PersistStatus[] = [
  'unchanged',
  'memoryUpdated',
  'persisted',
  'failed',
];

const STORE_OUTCOME: StoreOutcome[] = [
  'ok',
  'invalidId',
  'duplicateId',
  'notFound',
  'reservedId',
  'invalidField',
  'invalidHostMapping',
  'readOnly',
  'persistFailed',
  'refusedTabsOpen',
];

const HOST_MAPPING_FORMATS: HostMappingFormat[] = [
  'standardHosts',
  'whistleHost',
  'chromiumMap',
  'unsupported',
];

const HOST_MAPPING_APPLY_STATUSES: HostMappingApplyStatus[] = [
  'notRequested',
  'savedPending',
  'appliedToNetworkContext',
  'policyBlocked',
  'unknown',
  'failed',
];

function hostMappingFormatFromOrdinal(value: number): HostMappingFormat {
  return HOST_MAPPING_FORMATS[value] ?? 'unsupported';
}

function hostMappingFormatToOrdinal(value: HostMappingFormat): number {
  return HOST_MAPPING_FORMATS.indexOf(value);
}

/**
 * Converts a Mojo enum ordinal to its name.
 *
 * An out-of-range value becomes the most pessimistic option rather than throwing:
 * a newer browser that adds an outcome must not make an older page render "success".
 */
function persistStatusFromOrdinal(value: number): PersistStatus {
  return PERSIST_STATUS[value] ?? 'failed';
}

function storeOutcomeFromOrdinal(value: number): StoreOutcome {
  // `persistFailed` is the safe fallback: it makes the UI offer "try again" and never
  // claim success for an outcome it does not understand.
  return STORE_OUTCOME[value] ?? 'persistFailed';
}

// ---------------------------------------------------------------------- raw shapes

/**
 * The shapes the generated bindings actually deliver. Declared here rather than
 * imported because the generated .ts exposes them as part of the mojom namespace and
 * importing them adds a second spelling of every field name; this keeps the one
 * authoritative spelling in the interfaces above.
 */
interface RawEnvironmentInfo {
  id: string;
  name: string;
  color: string;
  icon: string;
  hostMappings: string;
  hostMappingFormat: number;
  enabled: boolean;
  isPersonal: boolean;
  tabCount: number;
}

interface RawEnvironmentList {
  environments: RawEnvironmentInfo[];
  readOnly: boolean;
  persistStatus: number;
  loadDiagnostics: string;
}

interface RawWriteResult {
  outcome: number;
  error: string;
  persistStatus: number;
  hostMappingApplyFailed: boolean;
  hostMappingApplyStatus: number;
  desiredHostMappingRevision: bigint;
  appliedHostMappingRevision: bigint;
  networkContextGeneration: bigint;
  routingPolicyReason: string;
  environment: RawEnvironmentInfo | null;
}

// -------------------------------------------------------------------- conversions

function toEnvironmentInfo(raw: RawEnvironmentInfo): EnvironmentInfo {
  return {
    id: raw.id,
    name: raw.name,
    color: raw.color,
    icon: raw.icon,
    hostMappings: raw.hostMappings,
    hostMappingFormat: hostMappingFormatFromOrdinal(raw.hostMappingFormat),
    enabled: raw.enabled,
    isPersonal: raw.isPersonal,
    tabCount: raw.tabCount,
  };
}

function toEnvironmentList(raw: RawEnvironmentList): EnvironmentList {
  return {
    // Personal is guaranteed to be first by the handler; sorting here as well would
    // hide a handler bug instead of surfacing it.
    environments: raw.environments.map(toEnvironmentInfo),
    readOnly: raw.readOnly,
    persistStatus: persistStatusFromOrdinal(raw.persistStatus),
    loadDiagnostics: raw.loadDiagnostics,
  };
}

function toWriteResult(raw: RawWriteResult): WriteResult {
  return {
    outcome: storeOutcomeFromOrdinal(raw.outcome),
    error: raw.error ?? '',
    persistStatus: persistStatusFromOrdinal(raw.persistStatus),
    hostMappingApplyFailed: raw.hostMappingApplyFailed,
    hostMappingApplyStatus:
      HOST_MAPPING_APPLY_STATUSES[raw.hostMappingApplyStatus] ?? 'unknown',
    desiredHostMappingRevision: raw.desiredHostMappingRevision,
    appliedHostMappingRevision: raw.appliedHostMappingRevision,
    networkContextGeneration: raw.networkContextGeneration,
    routingPolicyReason: raw.routingPolicyReason ?? '',
    environment: raw.environment ? toEnvironmentInfo(raw.environment) : null,
  };
}

/**
 * Turns a persist status into the durability the user cares about.
 *
 * `unchanged` maps to `disk` deliberately: it means "nothing was written by this
 * process", which happens when the value was already what the store holds (a no-op
 * save). Reporting `memory` there would tell the user their change is at risk when in
 * fact nothing was at risk and nothing changed.
 */
export function durabilityOf(status: PersistStatus): Durability {
  switch (status) {
    case 'persisted':
    case 'unchanged':
      return 'disk';
    case 'memoryUpdated':
      return 'memory';
    case 'failed':
      return 'failed';
  }
}

/**
 * Explains an outcome in words the user can act on.
 *
 * Returns `null` for `ok`, so a caller can use it as "is this a failure" without a
 * second comparison.
 */
export function describeOutcome(outcome: StoreOutcome): string | null {
  switch (outcome) {
    case 'ok':
      return null;
    case 'invalidId':
      return '环境 ID 无效。ID 只能包含小写字母、数字、连字符和下划线，长度 1-32 位。';
    case 'duplicateId':
      return '已经存在同 ID 的环境。请换一个 ID。';
    case 'notFound':
      return '找不到这个环境。它可能已经在别的窗口中被删除。';
    case 'reservedId':
      return '这是一个保留环境，不能执行该操作。';
    case 'invalidField':
      return '有字段没有通过校验。请检查名称长度和颜色格式。';
    case 'invalidHostMapping':
      return 'Host Mapping 规则无法解析。请对照下方的格式示例检查。';
    case 'readOnly':
      return '配置处于只读状态，无法写入。这通常是因为配置由更新版本的浏览器写入。';
    case 'persistFailed':
      return '写入失败，改动没有保存。';
    case 'refusedTabsOpen':
      return '这个环境还有标签页处于打开状态，请先关闭它们。';
  }
}

// ------------------------------------------------------------------------- client

/**
 * A thin, typed facade over the Mojo pipe.
 *
 * `bindPageHandler` is awaited once at startup. Everything else returns a Promise that
 * the UI awaits directly; there is no event bus, because there are no server-initiated
 * state changes other than "re-read the list".
 */
export class ParraApi {
  private handler: PageHandlerRemote;
  private callbackRouter: PageCallbackRouter;
  private factory: PageHandlerFactoryRemote;

  constructor() {
    this.handler = new PageHandlerRemote();
    this.callbackRouter = new PageCallbackRouter();
    this.factory = PageHandlerFactory.getRemote();
  }

  /**
   * Establishes the pipe. Must be called before any other method.
   *
   * SYNCHRONOUS, not a Promise. `createPageHandler` is a one-way mojom message: it has
   * no reply, so there is nothing to await, and declaring it async would only invite a
   * caller to believe the browser had accepted the handler by the time it resolved.
   * The first real acknowledgement is the first GetEnvironments() reply, which
   * ParraApp::start() awaits separately.
   */
  connect(): void {
    this.factory.createPageHandler(
      this.callbackRouter.$.bindNewPipeAndPassRemote(),
      this.handler.$.bindNewPipeAndPassReceiver(),
    );
  }

  /**
   * Registers the "something changed elsewhere" listener.
   *
   * The callback receives no payload: the page re-reads everything. That is a
   * deliberate trade. Forwarding the changed id would be cheaper but would let the page
   * render a partially updated state (one row new, one row stale), and this list is
   * small enough that a full re-read is not worth the inconsistency risk.
   */
  onEnvironmentsChanged(listener: () => void): void {
    this.callbackRouter.onEnvironmentsChanged.addListener(listener);
  }

  async getEnvironments(): Promise<EnvironmentList> {
    // EVERY mojom method with a reply returns `{result: T}`, not `T`. Reading
    // `raw.environments` directly yields undefined, and the first `.map()` on it throws
    // "Cannot read properties of undefined (reading 'map')" -- which the page then shows
    // as "could not read environments", correctly reporting a bug that is entirely in
    // this wrapper.
    const {result} = await this.handler.getEnvironments();
    return toEnvironmentList(result as unknown as RawEnvironmentList);
  }

  async getExternalLinkPolicy(): Promise<{
    policy: number; environmentId: string;
  }> {
    return await this.handler.getExternalLinkPolicy();
  }

  async setExternalLinkPolicy(
    policy: number, environmentId: string,
  ): Promise<{success: boolean; error: string}> {
    return await this.handler.setExternalLinkPolicy(policy, environmentId);
  }

  async createEnvironment(
    id: string,
    name: string,
    color: string,
    icon: string,
    hostMappings: string,
    hostMappingFormat: HostMappingFormat = 'standardHosts',
  ): Promise<WriteResult> {
    const {result} = await this.handler.createEnvironment(
      id,
      name,
      color,
      icon,
      hostMappings,
      hostMappingFormatToOrdinal(hostMappingFormat),
    );
    return toWriteResult(result as unknown as RawWriteResult);
  }

  /**
   * Partial update. Absent fields are passed as `has* = false` rather than as empty
   * strings, so "clear the colour" and "leave the colour alone" stay distinguishable --
   * the same reason the mojom uses a flag instead of an optional.
   */
  async updateEnvironment(
    id: string,
    patch: {name?: string; color?: string; icon?: string},
  ): Promise<WriteResult> {
    const {result} = await this.handler.updateEnvironment(
      id,
      patch.name !== undefined,
      patch.name ?? '',
      patch.color !== undefined,
      patch.color ?? '',
      patch.icon !== undefined,
      patch.icon ?? '',
    );
    return toWriteResult(result as unknown as RawWriteResult);
  }

  async setHostMappings(
    id: string,
    hostMappings: string,
    hostMappingFormat: HostMappingFormat = 'chromiumMap',
  ): Promise<WriteResult> {
    const {result} = await this.handler.setHostMappings(
      id, hostMappings, hostMappingFormatToOrdinal(hostMappingFormat));
    return toWriteResult(result as unknown as RawWriteResult);
  }

  async setEnabled(id: string, enabled: boolean): Promise<WriteResult> {
    const {result} = await this.handler.setEnabled(id, enabled);
    return toWriteResult(result as unknown as RawWriteResult);
  }

  async deleteEnvironment(id: string): Promise<WriteResult> {
    const {result} = await this.handler.deleteEnvironment(id);
    return toWriteResult(result as unknown as RawWriteResult);
  }
}
