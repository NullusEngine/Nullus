#!/usr/bin/env python3
"""Extract handbook C# examples and compile them against current engine APIs."""

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
from xml.sax.saxutils import escape

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dotnet", default="dotnet", help=".NET SDK executable (global.json applies)")
    parser.add_argument("--out", default="build/docs-acceptance/examples", help="generated validation projects")
    args = parser.parse_args()
    output = (ROOT / args.out).resolve()
    output.mkdir(parents=True, exist_ok=True)
    count = 0
    for language in ("zh-CN", "en"):
        project_dir = output / language
        project_dir.mkdir(parents=True, exist_ok=True)
        sources = []
        for page in sorted((ROOT / "Docs" / language).rglob("*.md")):
            text = page.read_text(encoding="utf-8")
            blocks = re.finditer(r"(?m)^```(?:csharp|cs)\s*\n([\s\S]*?)^```", text)
            for index, block in enumerate(blocks):
                source = block.group(1)
                if not re.search(r"\b(class|struct|interface|enum)\s+\w+", source):
                    source = ("using Nullus;\nnamespace Nullus.DocsExamples;\n"
                              f"internal static class Snippet{len(sources)} {{ public static void Verify() {{\n"
                              + source + "\n} }\n")
                filename = f"Example{len(sources)}.cs"
                line = text.count("\n", 0, block.start()) + 2
                source = f'#line {line} "{page.as_posix()}"\n' + source
                (project_dir / filename).write_text(source, encoding="utf-8")
                sources.append(filename)
                print(f"{page.relative_to(ROOT)}: C# block {index + 1}", flush=True)
        reference = escape((ROOT / "Managed/Nullus.Editor/Nullus.Editor.csproj").as_posix())
        includes = "".join(f'<Compile Include="{name}" />' for name in sources)
        project = (
            '<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup>'
            '<TargetFramework>net8.0</TargetFramework><Nullable>enable</Nullable>'
            '<ImplicitUsings>enable</ImplicitUsings><EnableDefaultCompileItems>false</EnableDefaultCompileItems>'
            '</PropertyGroup><ItemGroup>' + includes + f'<ProjectReference Include="{reference}" />'
            '</ItemGroup></Project>\n'
        )
        project_path = project_dir / "DocsExamples.csproj"
        project_path.write_text(project, encoding="utf-8")
        environment = dict(os.environ, DOTNET_NOLOGO="1", DOTNET_CLI_TELEMETRY_OPTOUT="1")
        result = subprocess.run([args.dotnet, "build", str(project_path), "--configuration", "Debug",
                                 "--verbosity", "minimal", "-m:1", "-p:BuildInParallel=false"],
                                cwd=ROOT, env=environment)
        if result.returncode:
            return result.returncode
        count += len(sources)
    print(f"compiled {count} C# examples from both languages")
    return 0


if __name__ == "__main__":
    sys.exit(main())
