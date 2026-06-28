# SOLVE

## NAME

solve - numerically solve one-variable equations and intersections

## SYNOPSIS

```
solve [options] 'EXPRESSION = EXPRESSION'
solve [options] 'EXPRESSION'
solve [options] --report-y 'Y1 = Y2'
```

## DESCRIPTION

`solve` finds real solutions for common high-school "solve for x" problems. It
is not a general symbolic algebra system, but `auto` has a bounded symbolic
front end for polynomial expressions through degree 2. Outside that subset, it
treats an equation as a numeric function and searches for values of one variable
that make the equation true.

For an input such as `x^2 - 2 = 0`, `solve` evaluates the left side minus the
right side and looks for roots of that zero function. Intersections are the same
problem: `x^2 = 2*x + 3` is solved as `x^2 - (2*x + 3) = 0`. With `--report-y`,
`solve` also reports the corresponding y-value for the left side of the
equation.

The tool is aimed at practical student workflows: checking algebra homework,
exploring where graphs cross the x-axis, finding intersections between two
curves, and seeing an approximate path toward the answer.

If no interval is supplied, `solve` scans a default visible school-math range
rather than failing the most natural invocation. The default is
`--scan -100:100:400`, followed by bisection on each bracketed sign change and
special handling for likely touching roots.

## CURRENT CAPABILITIES

- one real variable, defaulting to `x`
- equation input as `left = right`, or zero-function input as a single
  expression where solutions satisfy `expression = 0`
- intersection solving through the same equation form, with `--report-y` for
  y-value output
- arithmetic operators `+`, `-`, `*`, `/`, `%`, and `^`
- unary `+` and unary `-`
- parentheses for grouping
- decimal numeric constants
- predefined constants `pi` and `e`
- built-in functions such as `sqrt(x)`, `abs(x)`, `min(x, y)`, and `max(x, y)`
- math-library functions such as `sin(x)`, `cos(x)`, `atan(x)`, `log(x)`, and
  `exp(x)`
- interval solving with `--lo` and `--hi`
- automatic interval scanning to discover sign changes over a range
- automatic direct solving for polynomial equations through degree 2 when the
  expression can be represented in the supported polynomial subset
- simple factoring explanations for quadratics with simple rational-looking
  roots
- quadratic-formula solving for real quadratic roots
- polynomial identity detection for supported expressions that reduce to 0
- multiple-root reporting when scanning finds more than one candidate interval
- likely touching-root reporting for repeated roots that do not change sign
- didactic output that shows the chosen method, interval checks, iterations,
  approximations, and residual error
- JSON Lines output for scripted use

Deferred features include secant, Newton, arbitrary-precision decimal solving,
higher-degree polynomial factoring, general symbolic rearrangement,
non-polynomial identity proof, multi-variable solving, and a two-argument
`--intersect Y1 Y2` shortcut.

## OPTIONS

- `--var NAME` chooses the variable name; default is `x`
- `--lo VALUE` sets the lower endpoint of the search interval
- `--hi VALUE` sets the upper endpoint of the search interval
- `--scan LO:HI[:STEPS]` scans an interval before solving; `STEPS` defaults to
  400
- `--all` reports every candidate root found while scanning instead of stopping
  after the first root
- `--method bisection|auto` chooses the method; `auto` first handles simple
  linear equations directly, then falls back to scan plus bisection
- `--scale N` controls output precision from 0 through 18 displayed fractional
  digits. The default display scale is 10.
- `--tolerance VALUE` stops once the interval width or residual is small enough;
  default is `1e-10`
- `--max-iterations N` limits solver iterations; default is 128
- `--report-y` reports the y-value of the left side of an equation, useful for
  curve intersections
- `--explain` prints a didactic trace of the solving process
- `--quiet` prints only the final numeric answer
- `--json` writes machine-readable result and diagnostic events
- `--help` shows usage

`--scan` and `--lo`/`--hi` are mutually exclusive. `--scan` asks
the tool to discover candidate intervals; `--lo` and `--hi` provide one explicit
bracket. If neither form is supplied, `solve` behaves as if
`--scan -100:100:400` had been supplied.

## METHODS

`bisection` is the primary method. It requires an interval where the function
changes sign, such as `--lo 1 --hi 2` for `x^2 - 2 = 0`. It is slower than
Newton-style methods but predictable, robust, and easy to explain.

`auto` first tries to represent the transformed zero function as a polynomial
through degree 2. Linear equations are solved by moving the constant term and
dividing by the coefficient. Quadratics are solved directly; if the real roots
look like simple rational values, `--explain` also shows a factored form, and
otherwise it shows the quadratic formula path. Polynomial equations that reduce
to 0 are reported as identities. If the equation is outside that subset, `auto`
means "scan if needed, then use bisection on bracketed intervals." If an
explicit bracket is supplied, `auto` is equivalent to `bisection`.

Repeated roots such as `x^2 - 6*x + 9 = 0` do not change sign. A scan therefore
also looks for sampled points that are exact or near-zero, and for local minima
or maxima where `abs(f(x))` becomes small. These are reported as touching-root
candidates, with their residual, instead of silently saying no root was found.

Every reported root must pass a residual check. A sign change caused by a
discontinuity, such as `1/(x-2) = 0`, may appear bracketed, but it is not a root
if the final residual is not within tolerance. Such cases are rejected or
reported as suspected discontinuities.

## DIDACTIC OUTPUT

With `--explain`, `solve` should show the path toward the answer rather than
only the result. The trace is meant to be useful for learning, not just debugging
implementation internals.

A bisection explanation should include:

- the transformed zero function, for example `f(x) = (x^2 - 2) - 0`
- the starting interval and endpoint values
- why the interval brackets a root, or why it does not
- each midpoint approximation, function value, and retained interval
- the stopping condition that ended the search
- the final root approximation and residual

A linear explanation should show the equation after moving everything to one
side, the constant term being moved to the other side, division by the variable
coefficient, and the final answer.

A quadratic explanation should show the standard coefficients, discriminant,
quadratic formula, and, when the roots are simple rational-looking values, a
factored form.

A polynomial identity explanation should say that the transformed polynomial
reduced to all-zero coefficients and that the equation is true for every real
value of the variable.

For intersections, `--explain` should also show that the two input expressions
were converted into a single zero function, then report both `x` and `y` for the
intersection.

For touching-root candidates, `--explain` should say that no sign change was
found, show the near-zero local extremum or exact sampled zero that triggered
the candidate, and report the residual clearly.

## OUTPUT

Normal text output should be compact and stable. A single-root result should
include at least:

```
x = 1.4142135623
residual = -0.0000000001
method = bisection
iterations = 34
```

Roots that are very close to integers are printed compactly, such as `x = 5`.
When a non-integer root is close to a simple fraction with a small denominator,
normal text output includes a didactic hint in parentheses, such as
`x = 1.6666666667 (1 2/3)`.

For intersections, output should include the corresponding y-value:

```
x = 3
y = 9
residual = 0
method = bisection
iterations = 1
```

When `--quiet` is used, only the root value should be printed. When `--all` is
used, each result should be printed as a separate solution block.

If no root is found, normal output should say so and include the searched range
or interval. The exit status should distinguish success from failure:

- `0` at least one solution or candidate solution was found
- `1` no solution was found in the requested range
- `2` command-line usage or expression syntax error
- `3` numeric failure, such as division by zero during evaluation, overflow, or
  non-convergence after the iteration limit

## EXAMPLES

Find a square-root style zero:

```
solve --lo 1 --hi 2 'x^2 - 2 = 0'
```

Find the x-intercepts of a quadratic by scanning a range:

```
solve --scan -10:10:200 --all 'x^2 - 5*x + 6 = 0'
```

Find a repeated root where the graph only touches the x-axis:

```
solve --scan 0:6:120 'x^2 - 6*x + 9 = 0'
```

Find where a line and parabola intersect:

```
solve --report-y --scan -10:10 --all 'x^2 = 2*x + 3'
```

Use trigonometric functions numerically:

```
solve --lo 0 --hi 1 'cos(x) = x'
```

Show the bisection path:

```
solve --explain --method bisection --lo 1 --hi 2 'x^2 - 2 = 0'
```

Show a factored quadratic path:

```
solve --explain --all 'x^2 - 5*x + 6 = 0'
```

Show a polynomial identity:

```
solve --explain '(x + 1)^2 = x^2 + 2*x + 1'
```

## JSON Output

With `--json`, `solve` writes JSON Lines using the common envelope documented in
`json-output`. Events include:

- `solve_result` for each root or intersection found
- `solve_candidate` for likely touching roots that meet the candidate rules but
  were not bracketed by a sign change
- `solve_identity` for supported polynomial identities true for every real value
  of the variable
- `solve_summary` for method, status, and count information

A `solve_result` data object includes the variable name, root value, residual,
method, iteration count, and, for intersections, the y-value.
Diagnostics are written to stderr.

## LIMITATIONS

- no general symbolic algebra; `auto` can directly handle supported polynomial
  expressions through degree 2, including linear isolation, real quadratic roots,
  simple quadratic factoring explanations, and degree-2 polynomial identities,
  but it does not factor higher-degree polynomials, isolate variables in
  arbitrary expressions, or prove non-polynomial identities
- solves one variable at a time
- repeated roots and touching roots are only candidates unless the residual is
  within tolerance
- discontinuities can create false sign changes and must be reported carefully
- trigonometric and transcendental equations can have infinitely many roots;
  `solve` should only report roots in the requested interval or scan range
- very flat functions may converge slowly or be missed by coarse scanning
- current evaluation uses binary floating-point internally, so it is not yet an
  arbitrary-precision decimal solver like `bc`
- `solve` does not inherit `bc`'s normal `scale=0` integer-division default
- unknown identifiers other than the selected variable, predefined constants,
  and known function names should be errors rather than silently evaluating to 0
- equations with more than one variable are rejected

## IMPLEMENTATION NOTES

The current implementation uses a compact numeric expression evaluator local to
`solve`. A future implementation path is to extract the expression and
math-function evaluator currently used by `bc` into shared code, then build
solver methods on top of that evaluator. `solve` should configure any shared
calculator evaluator with a solver-appropriate nonzero scale and with `pi` and
`e` predefined.

Equation parsing has one important difference from `bc`: in `solve`, a single
top-level `=` means equation separation, not assignment. The command should find
one unparenthesized top-level `=` before handing either side to the expression
evaluator. It must not split `==`, `<=`, `>=`, or `!=`, and it should reject
ambiguous input with more than one top-level equation separator.

The implementation prioritizes correctness and clear diagnostics over symbolic
cleverness: default scanning, bracketed bisection, interval validation, residual
checks, touching-root candidates, clear no-root output, and tests against known
roots come before broad symbolic-looking features.

## SEE ALSO

bc, expr, awk
