#pragma once
#include <vector>
// DS3VR2 SHADERID (L7 2026-10-29): the game's pixel and compute shaders hashed at creation with 3Dmigoto's own hash
// (FNV-1 64, seed 0, over the bytecode - memory `migoto-hashes-reusable`), so a pass can be recognised by the name the
// geo11 fix gives it in `<game>\ShaderFixes\<hash>-ps.txt` / `-cs.txt`.
#include <cstddef>
#include <cstdint>

struct ID3D11Device;
struct ID3D11Resource;

namespace bvr::ds3::shaderid {

void set_dump_dir(const wchar_t* dir);   // SHADERDUMP: save every new shader's bytecode there (before install)
void install(ID3D11Device* dev);         // patch the device's CreatePixelShader / CreateComputeShader (once)
uint64_t hash_of(const void* shader);
bool gs_code(const void* gs, std::vector<uint8_t>& out);   // GSPROBE: a geometry shader's bytecode as created    // 3Dmigoto hash of a shader object the game created, 0 if not seen
const char* name_of(uint64_t hash);      // the geo11 fix's name for the passes we watch, else nullptr
// MARKERS (M8): the bytecode the game handed CreateVertexShader, needed to generate that shader's card geometry
// shader. The pointer stays valid while the shader object does; read it under no assumption of lifetime beyond the call.
bool vs_code(const void* shader, const void** code, size_t* len);
uint32_t vs_kept();                      // how many vertex shaders' bytecode we hold
uint32_t created();                      // shaders hashed so far
// TEXHASH: 3Dmigoto's hash of a texture the game created WITH initial data (the fix's [TextureOverride] names)
bool tex_hash(ID3D11Resource* r, uint32_t* hash);
uint32_t textures_hashed();
bool is_shaft_ps(const void* shader);   // SHAFTS: a pixel shader that uses the light-shaft constants
void report();                           // log how often each watched pass was created

} // namespace bvr::ds3::shaderid
