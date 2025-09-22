using CppAst;
using System;
using System.Collections.Generic;
using System.CommandLine;
using System.CommandLine.Parsing;
using System.IO;

class Program
{
    static int Main(string[] args)
    {
        var rootCommand = new RootCommand("C++ Code Generator");

        var includeOption = new Option<string[]>("--include","Include search paths");

        var defineOption = new Option<string[]>("--define","Preprocessor defines");

        var inputOption = new Option<string>("--input","Input C++ header or source file");

        var outputOption = new Option<string>("--output", "Output generated file");


        rootCommand.Add(includeOption);
        rootCommand.Add(defineOption);
        rootCommand.Add(inputOption);
        rootCommand.Add(outputOption);

        string text = @"
__cppast(MyNS::MyType<int>(1,2,3),MyNS::MyType<int>(1,2,3), A, B())
void function0();
int function1(int a, float b);
float function2(int);
";
        CppParserOptions op = new CppParserOptions();
        op.ParseTokenAttributes = true;
        var cppCompilation = CppParser.Parse(text, op);
        foreach (var c in cppCompilation.Classes)
        {
            Console.WriteLine($"Class: {c.Name}");
        }
        return 1;
    }
}