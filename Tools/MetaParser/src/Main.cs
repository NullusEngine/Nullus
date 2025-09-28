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



        string text = @"
#define B
class C;
class A{
    B
};

__cppast(MyNS::MyType<int>(1,2,3),MyNS::MyType<int>(1,2,3))
void function0();
int function1(int a, float b);
float function2(int);
";
        CppParserOptions op = new CppParserOptions();
        op.ParseTokenAttributes = true;
        op.EnableMacros();
        var cppCompilation = CppParser.Parse(text, op);
        foreach (var c in cppCompilation.Classes)
        {
            Console.WriteLine($"Class: {c.Name}");
        }
        return 1;
    }
}