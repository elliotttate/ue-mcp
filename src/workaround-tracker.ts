/**
 * Session-level tracker for execute_python workaround calls.
 * Both the editor tool (to push) and feedback tool (to read/flush) import this.
 */

export interface WorkaroundEntry {
  code: string;
  timestamp: string;
  resultSnippet?: string;
  /** What the caller said they were trying to do (searchable for #704 overlap report). */
  taskSummary?: string;
  /** If a dedicated action matched at execute_python time, "tool(action)". */
  suggestedTool?: string;
}

const stack: WorkaroundEntry[] = [];

export function pushWorkaround(entry: WorkaroundEntry): void {
  stack.push(entry);
}

export function getWorkarounds(): readonly WorkaroundEntry[] {
  return stack;
}

export function clearWorkarounds(): void {
  stack.length = 0;
}

export function workaroundCount(): number {
  return stack.length;
}
