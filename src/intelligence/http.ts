/**
 * Small fetch wrapper adding a per-request timeout and optional retry with
 * exponential backoff. External providers (embeddings, image gen, summarizer)
 * use this so a hung or rate-limited endpoint degrades into a clear error
 * instead of blocking a build forever.
 */
export interface HttpOptions {
  /** Retry attempts after the first try (default 0). */
  retries?: number;
  /** Abort a single attempt after this many ms (default 60s). */
  timeoutMs?: number;
  /** HTTP status codes worth retrying (default 429 + 5xx). */
  retryOn?: number[];
}

const DEFAULT_RETRY_ON = [429, 500, 502, 503, 504];

function backoffMs(attempt: number): number {
  return Math.min(8000, 500 * 2 ** attempt);
}

function delay(ms: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

export async function httpFetch(url: string, init: RequestInit, opts: HttpOptions = {}): Promise<Response> {
  const retries = opts.retries ?? 0;
  const timeoutMs = opts.timeoutMs ?? 60_000;
  const retryOn = opts.retryOn ?? DEFAULT_RETRY_ON;

  let lastError: unknown;
  for (let attempt = 0; attempt <= retries; attempt++) {
    try {
      const res = await fetch(url, { ...init, signal: AbortSignal.timeout(timeoutMs) });
      if (attempt < retries && retryOn.includes(res.status)) {
        await delay(backoffMs(attempt));
        continue;
      }
      return res;
    } catch (e) {
      lastError = e;
      if (attempt < retries) {
        await delay(backoffMs(attempt));
        continue;
      }
      throw e instanceof Error
        ? new Error(`request to ${new URL(url).host} failed: ${e.message}`)
        : e;
    }
  }
  throw lastError instanceof Error ? lastError : new Error(String(lastError));
}
