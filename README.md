# UE4 Load Monitor

This repository contains a small iOS development monitor and its Sileo package. The monitor detects the `ShadowTrackerExtra` process and generates an external UE4 reflection schema.

## Sileo source

Add this source:

`https://raw.githubusercontent.com/nreachit-source/stuck/main/`

Install the package named `UE4 Load Monitor`.

## What it does

1. **Process detection**: Polls the iOS process list for `ShadowTrackerExtra`.
2. **Marker file**: Writes `/var/mobile/Downloads/I_have_loaded.txt` on first detection.
3. **External SDK generation**: Reads UE4 reflection structures (UClass, UFunction, FProperty) from the running game process via Mach task ports and writes a JSON schema to `/var/mobile/Downloads/ue4_schema.json`.

The SDK generator is entirely **read-only** — it never writes to the game's memory.

## Source layout

### Daemon (`source/daemon/`)

| File | Purpose |
|---|---|
| `main.c` | Process monitor loop. Detects the target and triggers SDK generation. |
| `ue4_sdk.h/c` | Top-level SDK generator orchestrator. |
| `remote_memory.h/c` | Safe remote memory reading via `mach_vm_read_overwrite()`. |
| `aslr_slide.h/c` | Resolves the ASLR slide of the game's Mach-O image. |
| `pattern_scan.h/c` | Byte pattern scanner for locating engine globals. |
| `ue4_reflection.h/c` | Walks GUObjectArray and GNames/FNamePool to enumerate classes, properties, and functions. |
| `ue4_json.h/c` | Minimal JSON serializer matching the internal exporter's schema. |
| `ue4_offsets.h` | All UE4 struct field offsets in one file (single point of edit for version changes). |
| `entitlements.plist` | Daemon signing entitlements including `task_for_pid-allow`. |
| `com.local.ue4loadmonitor.plist` | Rootless launchd configuration. |

### Internal plugin (`source/plugin/UE4SchemaExporter/`)

Optional source-only UE4 plugin for projects built by their owner. Uses Unreal's public reflection API inside an authorized source build. Exports the same schema to `Saved/SchemaExport/ue4_schema.json`. Targets UE 4.25–4.27.

## Configuration

The external SDK generator looks for an optional configuration file at:

`/var/mobile/Downloads/ue4_sdk_config.txt`

Format (hex addresses are file offsets, pre-ASLR):

```
guobjectarray=0x1234ABCD
gnamepool=0x5678EFAB
```

If the file is absent or keys are missing, the generator attempts auto-detection via pattern scanning. Diagnostic output is written to `/var/mobile/Downloads/ue4_sdk.log`.

## Building

The daemon is an ARM64 iOS command-line Mach-O. Compile all `.c` files in `source/daemon/` against an iOS SDK:

```sh
zig cc -target aarch64-ios-none -std=c11 -O2 -Wall -Wextra \
    source/daemon/main.c \
    source/daemon/ue4_sdk.c \
    source/daemon/remote_memory.c \
    source/daemon/aslr_slide.c \
    source/daemon/pattern_scan.c \
    source/daemon/ue4_reflection.c \
    source/daemon/ue4_json.c \
    -o ue4loadmonitor
```

Sign with the included entitlements:

```sh
rcodesign sign --code-signature-flags runtime \
    --entitlements-xml-path source/daemon/entitlements.plist \
    ue4loadmonitor
```

Then add the CodeDirectory hash to Dopamine's trust cache, package into the `.deb`, and update the Sileo repository indexes.

## UE4 version

The struct offsets in `ue4_offsets.h` target UE 4.25–4.27 (ARM64). If the game uses a different engine version:

1. Edit `ue4_offsets.h` to match the target version's struct layouts.
2. If the engine is pre-4.25 (uses `UProperty` instead of `FProperty`), the `ChildProperties` linked list traversal in `ue4_reflection.c` must be changed to walk `UField::Next` instead of `FField::Next`.

## Output

The generated `ue4_schema.json` contains:

```json
{
  "format": "ue4-reflection-schema-v1",
  "scope": "type metadata only; no object values",
  "classes": [
    {
      "name": "/Script/Engine.Actor",
      "super": "/Script/CoreUObject.Object",
      "size": 560,
      "properties": [
        { "name": "bHidden", "type": "BoolProperty", "offset": 124, "element_size": 1, "array_dim": 1 }
      ],
      "functions": [
        { "name": "GetActorLocation", "flags": 67108864, "parameter_size": 12 }
      ]
    }
  ]
}
```

This is reflected type metadata only. No live object values, assets, credentials, or files are exported.
