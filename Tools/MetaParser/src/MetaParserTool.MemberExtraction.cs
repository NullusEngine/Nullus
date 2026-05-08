internal static partial class MetaParserTool
{
    private static List<ReflectFieldInfo> BuildAutoPropertyFields(
        string fullTypeName,
        List<MethodCandidateInfo> methods,
        HashSet<string> excludedMethodNames)
    {
        var getters = methods
            .Where(method => IsGetterCandidate(method) && !excludedMethodNames.Contains(method.Name))
            .GroupBy(GetPropertyKey, StringComparer.Ordinal)
            .ToDictionary(static group => group.Key, static group => SelectPreferredPropertyCandidate(group), StringComparer.Ordinal);

        var setters = methods
            .Where(method => IsSetterCandidate(method) && !excludedMethodNames.Contains(method.Name))
            .GroupBy(GetPropertyKey, StringComparer.Ordinal)
            .ToDictionary(static group => group.Key, static group => SelectPreferredPropertyCandidate(group), StringComparer.Ordinal);

        var fields = new List<ReflectFieldInfo>();
        foreach (var (propertyKey, getter) in getters)
        {
            setters.TryGetValue(propertyKey, out var setter);
            if (setter is null)
                continue;

            if (!ArePropertyTypesCompatible(getter.ReturnTypeName, setter.ParameterTypeNames[0]))
            {
                throw new InvalidOperationException(
                    $"Auto property '{GetPropertyName(getter)}' on '{fullTypeName}' has incompatible getter/setter types. " +
                    $"Getter returns '{getter.ReturnTypeName}', setter accepts '{setter.ParameterTypeNames[0]}'.");
            }

            var propertyName = GetPropertyName(getter);
            var fieldTypeName = NormalizePropertyTypeName(getter.ReturnTypeName);
            if (string.IsNullOrWhiteSpace(propertyName) || string.IsNullOrWhiteSpace(fieldTypeName))
                continue;
            if (ContainsUnsupportedReflectionType(fieldTypeName) || fieldTypeName.Contains('*', StringComparison.Ordinal))
                continue;

            fields.Add(new ReflectFieldInfo(propertyName, fieldTypeName, getter.PointerExpression, setter.PointerExpression, getter.IsPrivate || setter.IsPrivate));
        }

        return fields;
    }

    private static MethodCandidateInfo SelectPreferredPropertyCandidate(IEnumerable<MethodCandidateInfo> group)
    {
        return group
            .OrderBy(static method => ContainsUnsupportedReflectionType(NormalizePropertyTypeName(method.ReturnTypeName)) ? 1 : 0)
            .ThenBy(static method => NormalizePropertyTypeName(method.ReturnTypeName).Contains('*', StringComparison.Ordinal) ? 1 : 0)
            .ThenBy(static method => string.IsNullOrWhiteSpace(method.PropertyName) ? 1 : 0)
            .ThenByDescending(static method => method.Name.Length)
            .First();
    }

    private static bool IsGetterCandidate(MethodCandidateInfo method)
        => GetGetterPrefixLength(method.Name) > 0
           && method.ParameterTypeNames.Count == 0
           && !string.IsNullOrWhiteSpace(method.ReturnTypeName)
           && !string.Equals(method.ReturnTypeName, "void", StringComparison.Ordinal);

    private static bool IsSetterCandidate(MethodCandidateInfo method)
        => method.Name.StartsWith("Set", StringComparison.Ordinal)
           && method.Name.Length > 3
           && method.ParameterTypeNames.Count == 1;

    private static string GetPropertySuffix(MethodCandidateInfo method)
    {
        if (IsSetterCandidate(method))
            return method.Name["Set".Length..];

        var getterPrefixLength = GetGetterPrefixLength(method.Name);
        return getterPrefixLength > 0 ? method.Name[getterPrefixLength..] : method.Name;
    }

    private static string GetPropertyKey(MethodCandidateInfo method)
        => !string.IsNullOrWhiteSpace(method.PropertyName)
            ? method.PropertyName!
            : GetPropertySuffix(method);

    private static string GetPropertyName(MethodCandidateInfo method)
        => !string.IsNullOrWhiteSpace(method.PropertyName)
            ? method.PropertyName!
            : BuildPropertyNameFromSuffix(GetPropertySuffix(method));

    private static int GetGetterPrefixLength(string methodName)
    {
        if (methodName.StartsWith("Get", StringComparison.Ordinal) && methodName.Length > 3)
            return 3;
        if (methodName.StartsWith("Has", StringComparison.Ordinal) && methodName.Length > 3)
            return 3;
        if (methodName.StartsWith("Is", StringComparison.Ordinal) && methodName.Length > 2)
            return 2;

        return 0;
    }

    private static bool ArePropertyTypesCompatible(string getterType, string setterType)
        => string.Equals(NormalizePropertyTypeName(getterType), NormalizePropertyTypeName(setterType), StringComparison.Ordinal);
}
