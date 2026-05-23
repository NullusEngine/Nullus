#pragma once

#include "Object/Object.h"

#define ObjectVariant(object) NLS::meta::Variant { object, NLS::meta::variant_policy::WrapObject( ) }
