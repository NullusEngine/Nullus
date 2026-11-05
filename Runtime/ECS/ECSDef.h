#pragma once

#if defined(_WIN32) && defined(NLS_ECS_SHARED)
# if defined(NLS_ECS_EXPORT)
#  define NLS_ECS_API __declspec(dllexport)
# else
#  define NLS_ECS_API __declspec(dllimport)
# endif
#else
# define NLS_ECS_API
#endif
