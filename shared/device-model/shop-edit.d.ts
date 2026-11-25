// Types for shop-edit.js — the edits a person makes to a shop, written once. See the header there.
export type RefKind = 'gate' | 'board';
export interface Ref { where: string; holder: Record<string, unknown> | unknown[]; key: string | number; isKey: boolean }
export interface Dangling { kind: RefKind; id: string; where: string }
export function refsTo(doc: object, kind: RefKind, id: string): Ref[];
export function renameRefs(doc: object, kind: RefKind, from: string, to: string): void;
export function danglingRefs(doc: object): Dangling[];
export function unchain(doc: object, gateId: string): void;
export function unwireGate<T extends object>(doc: T, gateId: string): T;
export function wireGate<T extends object>(doc: T, gateId: string, boardId: string, where: { channel: number } | { bus: number }): T;
export function setServoNumber<T extends object>(doc: T, gateId: string, n: number): T;
/** Put an edited copy of an element back in its system, by id. False when no system holds it. */
export function replaceElement(doc: object, updated: { id: string }): boolean;
export function isWired(gate: { controllerId?: string } | null | undefined): boolean;
export function replaceBoard<T extends object>(doc: T, oldId: string, newId: string, newBoard?: Record<string, unknown>): T;
