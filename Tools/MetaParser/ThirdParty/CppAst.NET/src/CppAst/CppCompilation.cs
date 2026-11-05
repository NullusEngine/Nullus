// Copyright (c) Alexandre Mutel. All rights reserved.
// Licensed under the BSD-Clause 2 license.
// See license.txt file in the project root for full license information.

using System.Collections.Generic;

namespace CppAst
{
    /// <summary>A file buffer consumed by libclang, with its SHA-256 content fingerprint.</summary>
    public sealed record CppSourceFile(string Path, string ContentHash);

    /// <summary>
    /// The result of a compilation for a sets of C++ files.
    /// </summary>
    public class CppCompilation : CppGlobalDeclarationContainer
    {
        /// <summary>
        /// Constructor of this object.
        /// </summary>
        public CppCompilation()
        {
            Diagnostics = new CppDiagnosticBag();

            System = new CppGlobalDeclarationContainer();
        }

        /// <summary>
        /// Gets the attached diagnostic messages.
        /// </summary>
        public CppDiagnosticBag Diagnostics { get; }

        /// <summary>
        /// Gets the final input header text used by this compilation.
        /// </summary>
        public string InputText { get; set; }

        /// <summary>Transitive source inputs, populated when CollectSourceFiles is enabled.</summary>
        public List<CppSourceFile> SourceFiles { get; } = new List<CppSourceFile>();

        /// <summary>
        /// Gets a boolean indicating whether this instance has errors. See <see cref="Diagnostics"/> for more details.
        /// </summary>
        public bool HasErrors => Diagnostics.HasErrors;

        /// <summary>
        /// Gets all the declarations that are coming from system include folders used by the declarations in this object.
        /// </summary>
        public CppGlobalDeclarationContainer System { get; }
    }
}
