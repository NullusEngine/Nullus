export module nullus.math.pilot;

export namespace NLS::Maths::ModulesPilot
{
    constexpr int Add(int lhs, int rhs)
    {
        return lhs + rhs;
    }

    constexpr int ClampNonNegative(int value)
    {
        return value < 0 ? 0 : value;
    }
}
