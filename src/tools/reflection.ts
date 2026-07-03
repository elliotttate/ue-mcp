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
    create_struct:  bp("Create a UUserDefinedStruct asset (datatable row structs, typed BP variables). Params: name, packagePath?, members: {name, type, default?, tooltip?, isArray?}[] ('variables'/'fields' accepted as aliases), onConflict?. Types accept the blueprint variable syntax incl. containers (array:float, set:name, map:string,int, enum:/Game/E_Foo)", "create_struct", (p) => ({ name: p.name, packagePath: p.packagePath, members: p.members ?? p.variables ?? p.fields, onConflict: p.onConflict })),
    set_struct_members: bp("Replace all members on an existing UUserDefinedStruct. Params: assetPath, members[] (aliases: variables/fields)", "set_struct_members", (p) => ({ assetPath: p.assetPath, members: p.members ?? p.variables ?? p.fields })),
    set_struct_fields: bp("Alias for set_struct_members (kept for compatibility). Params: assetPath, variables: {name, type, isArray?}[]", "set_struct_fields", (p) => ({ assetPath: p.assetPath, members: p.members ?? p.variables ?? p.fields })),
    read_struct_members: bp("List a UUserDefinedStruct's members with friendly names, types, guids, defaults, and compile status. Params: assetPath", "read_struct_members", (p) => ({ assetPath: p.assetPath })),
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
    members: z.array(z.object({
      name: z.string(),
      type: z.string().describe("Blueprint variable type syntax: float, int, bool, string, Vector, Actor, enum:/Game/E_Foo, array:float, map:string,int, ..."),
      default: z.string().optional(),
      tooltip: z.string().optional(),
      isArray: z.boolean().optional().describe("Sugar for array:<type>"),
    })).optional().describe("Struct members (create_struct / set_struct_members)"),
    variables: z.array(
      z.object({ name: z.string(), type: z.string(), isArray: z.boolean().optional() }),
    ).optional().describe("Alias for members"),
    fields: z.array(
      z.object({ name: z.string(), type: z.string(), isArray: z.boolean().optional() }),
    ).optional().describe("Alias for members"),
    onConflict: z.string().optional().describe("Asset-creation conflict policy: skip (default) | error | overwrite"),
  },
);
