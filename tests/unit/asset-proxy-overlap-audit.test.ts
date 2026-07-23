import { describe, expect, it, vi } from "vitest";
import { readFileSync } from "node:fs";
import { assetTool } from "../../src/tools/asset.js";
import type { ToolContext } from "../../src/types.js";

describe("asset proxy_overlap_audit", () => {
  it("exposes positive tolerance/example schema and maps the read-only bridge call", async () => {
    const call = vi.fn(async () => ({ success: true }));
    const ctx = { bridge: { call } } as unknown as ToolContext;
    const parsed = assetTool.schema.action.safeParse("proxy_overlap_audit");
    expect(parsed.success).toBe(true);
    expect(assetTool.schema.toleranceCm.safeParse(0.001).success).toBe(true);
    expect(assetTool.schema.toleranceCm.safeParse(0).success).toBe(false);
    expect(assetTool.schema.maxExamples.safeParse(12).success).toBe(true);
    expect(assetTool.schema.maxExamples.safeParse(1.5).success).toBe(false);
    expect(assetTool.schema.lodIndex.safeParse(2).success).toBe(true);
    expect(assetTool.schema.lodIndex.safeParse(-1).success).toBe(false);
    expect(assetTool.schema.lodIndex.safeParse(1.5).success).toBe(false);
    expect(assetTool.schema.offset.safeParse(25).success).toBe(true);
    expect(assetTool.schema.offset.safeParse(-1).success).toBe(false);

    await assetTool.handler(ctx, {
      action: "proxy_overlap_audit",
      directory: "/Game/Merged/Canyon",
      lodIndex: 2,
      toleranceCm: 0.002,
      recursive: true,
      offset: 25,
      maxResults: 50,
      maxExamples: 8,
    });

    expect(call).toHaveBeenCalledWith("proxy_overlap_audit", {
      assetPath: undefined,
      directory: "/Game/Merged/Canyon",
      lodIndex: 2,
      toleranceCm: 0.002,
      recursive: true,
      offset: 25,
      maxResults: 50,
      maxExamples: 8,
    }, undefined);
  });

  it("reads built render-buffer counts instead of source mesh-description counts", () => {
    const source = readFileSync(new URL(
      "../../plugin/ue_mcp_bridge/Source/UE_MCP_Bridge/Private/Handlers/AssetHandlers_ProxyAudit.cpp",
      import.meta.url,
    ), "utf8");

    expect(source).toContain("Mesh->GetNumTriangles(LodIndex)");
    expect(source).toContain("Mesh->GetNumVertices(LodIndex)");
    expect(source).toContain('TEXT("builtRenderTriangleCount")');
    expect(source).toContain('TEXT("builtRenderVertexCount")');
    expect(source).toContain('TEXT("builtRenderLodAvailable")');
  });
});
