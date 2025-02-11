/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __ANDROID_DLEXT_NAMESPACES_H__
#define __ANDROID_DLEXT_NAMESPACES_H__

#include <android/dlext.h>
#include <stdbool.h>

__BEGIN_DECLS

enum {
  /* A regular namespace is the namespace with a custom search path that does
   * not impose any restrictions on the location of native libraries.
   */
  ANDROID_NAMESPACE_TYPE_REGULAR = 0,

  /* An isolated namespace requires all the libraries to be on the search path
   * or under permitted_when_isolated_path. The search path is the union of
   * ld_library_path and default_library_path.
   */
  ANDROID_NAMESPACE_TYPE_ISOLATED = 1,

  /* The shared namespace clones the list of libraries of the caller namespace upon creation
   * which means that they are shared between namespaces - the caller namespace and the new one
   * will use the same copy of a library if it was loaded prior to android_create_namespace call.
   *
   * Note that libraries loaded after the namespace is created will not be shared.
   *
   * Shared namespaces can be isolated or regular. Note that they do not inherit the search path nor
   * permitted_path from the caller's namespace.
   */
  ANDROID_NAMESPACE_TYPE_SHARED = 2,

  /* This flag instructs linker to enable grey-list workaround for the namespace.
   * See http://b/26394120 for details.
   */
  ANDROID_NAMESPACE_TYPE_GREYLIST_ENABLED = 0x08000000,

  /* This flag instructs linker to use this namespace as the anonymous
   * namespace. The anonymous namespace is used in the case when linker cannot
   * identify the caller of dlopen/dlsym. This happens for the code not loaded
   * by dynamic linker; for example calls from the mono-compiled code. There can
   * be only one anonymous namespace in a process. If there already is an
   * anonymous namespace in the process, using this flag when creating a new
   * namespace causes an error.
   */
  ANDROID_NAMESPACE_TYPE_ALSO_USED_AS_ANONYMOUS = 0x10000000,

  ANDROID_NAMESPACE_TYPE_SHARED_ISOLATED =
      ANDROID_NAMESPACE_TYPE_SHARED | ANDROID_NAMESPACE_TYPE_ISOLATED,
};

/*
 * Creates new linker namespace.
 * ld_library_path and default_library_path represent the search path
 * for the libraries in the namespace.
 *
 * The libraries in the namespace are searched by folowing order:
 * 1. ld_library_path (Think of this as namespace-local LD_LIBRARY_PATH)
 * 2. In directories specified by DT_RUNPATH of the "needed by" binary.
 * 3. deault_library_path (This of this as namespace-local default library path)
 *
 * When type is ANDROID_NAMESPACE_TYPE_ISOLATED the resulting namespace requires all of
 * the libraries to be on the search path or under the permitted_when_isolated_path;
 * the search_path is ld_library_path:default_library_path. Note that the
 * permitted_when_isolated_path path is not part of the search_path and
 * does not affect the search order. It is a way to allow loading libraries from specific
 * locations when using absolute path.
 * If a library or any of its dependencies are outside of the permitted_when_isolated_path
 * and search_path, and it is not part of the public namespace dlopen will fail.
 */
extern struct android_namespace_t* android_create_namespace(
    const char* name, const char* ld_library_path, const char* default_library_path, uint64_t type,
    const char* permitted_when_isolated_path, struct android_namespace_t* parent);

/*
 * Creates a link between namespaces. Every link has list of sonames of
 * shared libraries. These are the libraries which are accessible from
 * namespace 'from' but loaded within namespace 'to' context.
 * When to namespace is nullptr this function establishes a link between
 * 'from' namespace and the default namespace.
 *
 * The lookup order of the libraries in namespaces with links is following:
 * 1. Look inside current namespace using 'this' namespace search path.
 * 2. Look in linked namespaces
 * 2.1. Perform soname check - if library soname is not in the list of shared
 *      libraries sonames skip this link, otherwise
 * 2.2. Search library using linked namespace search path. Note that this
 *      step will not go deeper into linked namespaces for this library but
 *      will do so for DT_NEEDED libraries.
 */
extern bool android_link_namespaces(struct android_namespace_t* from,
                                    struct android_namespace_t* to,
                                    const char* shared_libs_sonames);

extern struct android_namespace_t* android_get_exported_namespace(const char* name);

__END_DECLS

#endif /* __ANDROID_DLEXT_NAMESPACES_H__ */
