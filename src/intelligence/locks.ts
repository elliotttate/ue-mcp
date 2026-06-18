/**
 * In-process advisory locks so two concurrent operations cannot corrupt the
 * same on-disk index. MCP clients can issue overlapping tool calls; without
 * this, two `index build`s would race on the vector store and manifest.
 */
const inFlight = new Set<string>();

export async function withProjectLock<T>(key: string, fn: () => Promise<T>): Promise<T> {
  if (inFlight.has(key)) {
    throw new Error(`Another operation (${key}) is already running for this project. Wait for it to finish.`);
  }
  inFlight.add(key);
  try {
    return await fn();
  } finally {
    inFlight.delete(key);
  }
}
