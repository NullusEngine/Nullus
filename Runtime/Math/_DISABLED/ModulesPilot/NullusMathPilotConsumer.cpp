import nullus.math.pilot;

static_assert(NLS::Maths::ModulesPilot::Add(2, 3) == 5);
static_assert(NLS::Maths::ModulesPilot::ClampNonNegative(-4) == 0);
static_assert(NLS::Maths::ModulesPilot::ClampNonNegative(7) == 7);

int NullusMathPilotConsumer()
{
    return NLS::Maths::ModulesPilot::Add(20, 23);
}
