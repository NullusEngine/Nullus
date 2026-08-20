using System;
using System.Collections.Immutable;
using System.Composition;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CodeActions;
using Microsoft.CodeAnalysis.CodeFixes;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;

namespace Nullus.ScriptDebugger;

[ExportCodeFixProvider(LanguageNames.CSharp, Name = nameof(NullusCodeFixProvider)), Shared]
public sealed class NullusCodeFixProvider : CodeFixProvider
{
    private static readonly ImmutableArray<string> FixIds = ImmutableArray.Create("NLS100", "NLS101", "NLS102", "NLS105");

    public override ImmutableArray<string> FixableDiagnosticIds => FixIds;
    public override FixAllProvider GetFixAllProvider() => WellKnownFixAllProviders.BatchFixer;

    public override async Task RegisterCodeFixesAsync(CodeFixContext context)
    {
        var diagnostic = context.Diagnostics.FirstOrDefault();
        if (diagnostic == null)
            return;
        var root = await context.Document.GetSyntaxRootAsync(context.CancellationToken).ConfigureAwait(false);
        if (root == null)
            return;

        if (diagnostic.Id == "NLS100")
        {
            var declaration = root.FindNode(diagnostic.Location.SourceSpan).FirstAncestorOrSelf<ClassDeclarationSyntax>();
            if (declaration != null)
            {
                var name = Path.GetFileNameWithoutExtension(context.Document.FilePath ?? declaration.Identifier.Text);
                context.RegisterCodeFix(CodeAction.Create(
                    "Rename Behaviour to match file",
                    cancellationToken => RenameTypeAsync(context.Document, declaration, name, cancellationToken),
                    "NullusRenameBehaviour"), diagnostic);
            }
        }
        else if (diagnostic.Id == "NLS101" || diagnostic.Id == "NLS102")
        {
            var method = root.FindNode(diagnostic.Location.SourceSpan).FirstAncestorOrSelf<MethodDeclarationSyntax>();
            if (method != null)
            {
                context.RegisterCodeFix(CodeAction.Create(
                    diagnostic.Id == "NLS102" ? "Remove async from lifecycle method" : "Fix Nullus lifecycle signature",
                    cancellationToken => NormalizeLifecycleAsync(context.Document, method, cancellationToken),
                    diagnostic.Id == "NLS102" ? "NullusRemoveAsyncLifecycle" : "NullusFixLifecycleSignature"), diagnostic);
            }
        }
        else if (diagnostic.Id == "NLS105")
        {
            var attribute = root.FindNode(diagnostic.Location.SourceSpan).FirstAncestorOrSelf<AttributeSyntax>();
            if (attribute != null)
            {
                context.RegisterCodeFix(CodeAction.Create(
                    "Remove invalid FormerlySerializedAs",
                    cancellationToken => RemoveAttributeAsync(context.Document, attribute, cancellationToken),
                    "NullusRemoveInvalidFormerlySerializedAs"), diagnostic);
            }
        }
    }

    private static async Task<Document> RenameTypeAsync(Document document, ClassDeclarationSyntax declaration, string name, CancellationToken cancellationToken)
    {
        var root = await document.GetSyntaxRootAsync(cancellationToken).ConfigureAwait(false);
        if (root == null)
            return document;
        return document.WithSyntaxRoot(root.ReplaceToken(declaration.Identifier, SyntaxFactory.Identifier(declaration.Identifier.LeadingTrivia, name, declaration.Identifier.TrailingTrivia)));
    }

    private static async Task<Document> NormalizeLifecycleAsync(Document document, MethodDeclarationSyntax method, CancellationToken cancellationToken)
    {
        var root = await document.GetSyntaxRootAsync(cancellationToken).ConfigureAwait(false);
        if (root == null)
            return document;
        var modifiers = method.Modifiers
            .Where(token => !token.IsKind(SyntaxKind.AsyncKeyword)
                && !token.IsKind(SyntaxKind.StaticKeyword)
                && !token.IsKind(SyntaxKind.PrivateKeyword)
                && !token.IsKind(SyntaxKind.ProtectedKeyword)
                && !token.IsKind(SyntaxKind.InternalKeyword)
                && !token.IsKind(SyntaxKind.PublicKeyword)
                && !token.IsKind(SyntaxKind.OverrideKeyword))
            .ToList();
        modifiers.Insert(0, SyntaxFactory.Token(SyntaxKind.PublicKeyword));
        modifiers.Insert(1, SyntaxFactory.Token(SyntaxKind.OverrideKeyword));
        var normalized = method
            .WithModifiers(SyntaxFactory.TokenList(modifiers))
            .WithReturnType(SyntaxFactory.PredefinedType(SyntaxFactory.Token(SyntaxKind.VoidKeyword)))
            .WithParameterList(SyntaxFactory.ParameterList());
        return document.WithSyntaxRoot(root.ReplaceNode(method, normalized));
    }

    private static async Task<Document> RemoveAttributeAsync(Document document, AttributeSyntax attribute, CancellationToken cancellationToken)
    {
        var root = await document.GetSyntaxRootAsync(cancellationToken).ConfigureAwait(false);
        if (root == null)
            return document;
        if (attribute.Parent is not AttributeListSyntax list)
            return document;
        if (list.Attributes.Count == 1)
        {
            var updatedRoot = root.RemoveNode(list, SyntaxRemoveOptions.KeepNoTrivia);
            return document.WithSyntaxRoot(updatedRoot ?? root);
        }
        return document.WithSyntaxRoot(root.ReplaceNode(list, list.WithAttributes(list.Attributes.Remove(attribute))));
    }
}
