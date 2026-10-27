nls_add_runtime_target(
  NAME NLS_VFXCompiler
  NO_REFLECTION
  SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}
  LIBS NLS_VFX
)

set(_particle_sprite_source "${CMAKE_SOURCE_DIR}/App/Assets/Engine/Shaders/NullusShaderLibrary/ParticleSprite.hlsl")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_particle_sprite_source}")
file(READ "${_particle_sprite_source}" NLS_PARTICLE_SPRITE_SOURCE)
configure_file(ParticleSpriteSource.h.in "${CMAKE_CURRENT_BINARY_DIR}/ParticleSpriteSource.h" @ONLY)
target_include_directories(NLS_VFXCompiler PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
