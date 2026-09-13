# UE4 Schema Exporter

This runtime plugin exports reflected type metadata from an application you build from source. It does not inspect another process and does not export live object values.

Copy this directory into the UE project's `Plugins` directory, enable `UE4 Schema Exporter`, and compile the iOS target. After engine initialization it writes:

`Saved/SchemaExport/ue4_schema.json`

The JSON contains reflected class names, superclass names, structure sizes, declared functions, and declared property types/offsets.

This implementation targets UE 4.25–4.27, where reflected properties use `FProperty`. Older UE4 releases need the property iterator changed to `UProperty`.
