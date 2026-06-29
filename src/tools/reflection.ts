import { z } from "zod";
import { categoryTool, bp, type ToolDef } from "../types.js";

export const reflectionTool: ToolDef = categoryTool(
  "reflection",
  "UE reflection: classes, structs, enums, gameplay tags.",
  {
    reflect_class:  bp("Reflect UClass. Params: className, includeInherited?", "reflect_class"),
    reflect_struct: bp("Reflect UScriptStruct. Params: structName", "reflect_struct"),
    reflect_enum:   bp("Reflect UEnum. Params: enumName", "reflect_enum"),
    list_classes:   bp("List classes. Params: parentFilter?, limit?", "list_classes"),
    list_tags:      bp("List gameplay tags. Params: filter?", "list_gameplay_tags"),
    create_tag:     bp("Create gameplay tag. Params: tag, comment?", "create_gameplay_tag"),
    create_enum:    bp("Create UUserDefinedEnum asset, optionally seeded with entries. Params: name, packagePath?, entries?: (string|{name, displayName?})[], onConflict? (#274)", "create_enum", (p) => ({ name: p.name, packagePath: p.packagePath, entries: p.entries, onConflict: p.onConflict })),
    set_enum_entries: bp("Replace entries on an existing UUserDefinedEnum. Params: assetPath, entries[] (#274)", "set_enum_entries", (p) => ({ assetPath: p.assetPath, entries: p.entries })),
    create_struct:  bp("Create UUserDefinedStruct asset seeded with fields. Params: name, packagePath?, variables: {name, type, isArray?}[], onConflict?. Field types accept the same strings as blueprint add_variable (float, FVector, Actor, E_MyEnum, /Game/Path/S_Other, soft-refs).", "create_struct", (p) => ({ name: p.name, packagePath: p.packagePath, variables: p.variables ?? p.fields, onConflict: p.onConflict })),
    set_struct_fields: bp("Replace the field list on an existing UUserDefinedStruct. Params: assetPath, variables: {name, type, isArray?}[]", "set_struct_fields", (p) => ({ assetPath: p.assetPath, variables: p.variables ?? p.fields })),
    search_functions: bp("Live search for BlueprintCallable/Pure functions by keyword (ranked over name, params, return type, owning class, tooltip). Use before authoring logic to get correct signatures. Params: query, classFilter?, limit?", "search_functions", (p) => ({ query: p.query, classFilter: p.classFilter, limit: p.limit })),
  },
  undefined,
  {
    className: z.string().optional(),
    includeInherited: z.boolean().optional(),
    structName: z.string().optional(),
    enumName: z.string().optional(),
    parentFilter: z.string().optional(),
    limit: z.number().optional(),
    filter: z.string().optional(),
    query: z.string().optional().describe("Keyword query (search_functions)"),
    classFilter: z.string().optional().describe("Restrict search_functions to this class + its supers"),
    tag: z.string().optional(),
    comment: z.string().optional(),
    name: z.string().optional().describe("Enum asset name (create_enum)"),
    packagePath: z.string().optional().describe("Package path (default /Game)"),
    assetPath: z.string().optional().describe("Existing UserDefinedEnum path (set_enum_entries)"),
    entries: z.array(z.union([
      z.string(),
      z.object({ name: z.string(), displayName: z.string().optional() }),
    ])).optional().describe("Enum entries — strings or {name, displayName?}"),
    variables: z.array(
      z.object({ name: z.string(), type: z.string(), isArray: z.boolean().optional() }),
    ).optional().describe("Struct fields — {name, type, isArray?} (create_struct / set_struct_fields)"),
    fields: z.array(
      z.object({ name: z.string(), type: z.string(), isArray: z.boolean().optional() }),
    ).optional().describe("Alias for variables"),
    onConflict: z.string().optional().describe("Asset-creation conflict policy: skip (default) | error | overwrite"),
  },
);
