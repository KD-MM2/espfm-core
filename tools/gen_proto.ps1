# Regenerate nanopb bindings (firmware C code)
& 'C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1'
python C:/Espressif/tools/python/v6.0.1/venv/Lib/site-packages/nanopb/generator/nanopb_generator.py `
    -I components/f_schema/proto `
    -I components/f_schema `
    -D components/f_schema/ `
    components/f_schema/proto/espfm.proto

# Regenerate Python protobuf bindings (for espfm_shell.py)
python -m grpc_tools.protoc `
    -I components/f_schema/proto `
    --python_out=tools `
    components/f_schema/proto/espfm.proto

# Copy source-of-truth proto to Rust crate (prepend `package espfm;` for prost)
$src = Get-Content "components/f_schema/proto/espfm.proto" -Raw
$dest = "espfm-gui/crates/espfm-coap/proto/espfm.proto"
Set-Content $dest "syntax = `"proto3`";`npackage espfm;`n`n$($src.Substring($src.IndexOf("`n") + 1))" -NoNewline
