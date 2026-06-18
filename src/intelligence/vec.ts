/**
 * Small dense-vector helpers shared by the embedding providers and the
 * vector store. Vectors are plain Float32Array; we store them L2-normalized
 * so cosine similarity reduces to a dot product.
 */

/** Dot product of two equal-length (or min-length) vectors. */
export function dot(a: Float32Array, b: Float32Array): number {
  let s = 0;
  const n = Math.min(a.length, b.length);
  for (let i = 0; i < n; i++) s += a[i] * b[i];
  return s;
}

/** In-place L2 normalization. No-op on the zero vector. */
export function l2normalize(v: Float32Array): void {
  let s = 0;
  for (let i = 0; i < v.length; i++) s += v[i] * v[i];
  const norm = Math.sqrt(s);
  if (norm > 0) {
    for (let i = 0; i < v.length; i++) v[i] /= norm;
  }
}

/**
 * Hashing-trick accumulation: place `token` into one of `dim` buckets with a
 * decorrelated +/- sign, weighted by `weight`. Used by the local embedding to
 * project a bag of (sub)words into a fixed-dimensional space without a model.
 * The sign hash is salted so it does not correlate with the bucket hash.
 */
export function addHashed(
  v: Float32Array,
  token: string,
  weight: number,
  dim: number,
  hash: (s: string) => number,
): void {
  const bucket = hash(token) % dim;
  const sign = (hash(token + "sign") & 1) === 1 ? 1 : -1;
  v[bucket] += weight * sign;
}
