# This directory intentionally owns the generation rule independently of the
# consuming library. Input/output paths and working directory are supplied by
# nls_add_meta_generation; no compiler flags or generated contents change here.
nls_define_meta_generation_command()
add_custom_target(${_generation_target} ${_generation_all_option}
    DEPENDS "${_gen_stamp}" "${_gen_cpp}" "${_gen_h}" ${_meta_generated_side_outputs})
set_target_properties(${_generation_target} PROPERTIES
    FOLDER "CMakePredefinedTargets/Reflection")
