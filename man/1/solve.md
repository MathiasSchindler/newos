# SOLVE

## NAME

solve - solve one-variable equations and intersections

## SYNOPSIS

```
solve [OPTIONS] 'EXPRESSION = EXPRESSION'
solve [OPTIONS] 'EXPRESSION'
solve [OPTIONS] --report-y 'Y1 = Y2'
```

## DESCRIPTION

`solve` finds real solutions for common high-school "solve for x" problems. It is not a general symbolic algebra system, but `auto` has a bounded symbolic front end for polynomial expressions through degree 16. Outside that subset, it treats an equation as a numeric function and searches for values of one variable that make the equation true.

For an input such as `x^2 - 2 = 0`, `solve` evaluates the left side minus the right side and looks for roots of that zero function. Intersections are the same problem: `x^2 = 2*x + 3` is solved as `x^2 - (2*x + 3) = 0`. With `--report-y`, `solve` also reports the corresponding y-value for the left side of the equation.

The tool is aimed at practical student workflows: checking algebra homework, exploring where graphs cross the x-axis, finding intersections between two curves, and seeing an approximate path toward the answer.

If no interval is supplied, `solve` scans a default visible school-math range rather than failing the most natural invocation. The default is `--scan -100:100:400`, followed by bisection on each bracketed sign change and special handling for likely touching roots.

## CURRENT CAPABILITIES

- one real variable, defaulting to `x`
- equation input as `left = right`, or zero-function input as a single expression where solutions satisfy `expression = 0`
- intersection solving through the same equation form, with `--report-y` for y-value output
- arithmetic operators `+`, `-`, `*`, `/`, `%`, and `^`
- unary `+` and unary `-`
- parentheses for grouping
- decimal numeric constants
- predefined constants `pi` and `e`
- built-in functions such as `sqrt(x)`, `abs(x)`, `min(x, y)`, and `max(x, y)`
- math-library functions such as `sin(x)`, `cos(x)`, `atan(x)`, `log(x)`, and `exp(x)`
- interval solving with `--lo` and `--hi`
- automatic interval scanning to discover sign changes over a range
- automatic direct solving for polynomial equations through degree 16 when the expression can be represented in the supported polynomial subset
- simple factoring explanations for quadratics with simple rational-looking roots
- quadratic-formula solving for real quadratic roots
- rational-root factoring for higher-degree polynomials in the supported subset
- exact polynomial identity detection for integer-literal polynomial expressions and approximate identity reporting for decimal-coefficient polynomial expressions that reduce to 0 within tolerance
- multiple-root reporting when scanning finds more than one candidate interval
- likely touching-root reporting for repeated roots that do not change sign
- didactic output that shows the chosen method, interval checks, iterations, approximations, and residual error
- JSON Lines output for scripted use

## OPTIONS

- `--var NAME` chooses the variable name; default is `x`
- `--lo VALUE` sets the lower endpoint of the search interval
- `--hi VALUE` sets the upper endpoint of the search interval
- `--scan LO:HI[:STEPS]` scans an interval before solving; `STEPS` defaults to 400
- `--all` reports every candidate root found while scanning instead of stopping after the first root
- `--method bisection|auto` chooses the method; `auto` first tries the supported polynomial front end, then falls back to scan plus bisection
- `--scale N` controls output precision from 0 through 15 displayed fractional digits; the default display scale is 10
- `--tolerance VALUE` stops once the interval width or residual is small enough; the default is `1e-10`
- `--max-iterations N` limits solver iterations; the default is 128
- `--report-y` reports the y-value of the left side of an equation, useful for curve intersections
- `--explain` prints a didactic trace of the solving process
- `--quiet` prints only the final answer
- `--json` writes machine-readable result and diagnostic events
- `--help` shows usage

`--scan` and `--lo`/`--hi` are mutually exclusive. `--scan` asks the tool to discover candidate intervals; `--lo` and `--hi` provide one explicit bracket. If neither form is supplied, `solve` behaves as if `--scan -100:100:400` had been supplied.

## METHODS

`bisection` is the primary numeric method. It requires an interval where the function changes sign, such as `--lo 1 --hi 2` for `x^2 - 2 = 0`. It is slower than Newton-style methods but predictable, robust, and easy to explain.

`auto` first tries to represent the transformed zero function as a polynomial through degree 16. Linear equations are solved by moving the constant term and dividing by the coefficient. Quadratics are solved directly; if the real roots look like simple rational values, `--explain` also shows a factored form, and otherwise it shows the quadratic formula path. Higher-degree polynomials in the supported subset are factored when rational roots can be found; any remaining quadratic factor is solved with the quadratic formula. Integer-literal polynomial equations that reduce to 0 are reported as exact identities; decimal-coefficient polynomial equations that reduce to 0 within tolerance are reported as approximate identities. If the equation is outside that subset, `auto` means "scan if needed, then use bisection on bracketed intervals." If an explicit bracket is supplied, `auto` is equivalent to `bisection`.

Repeated roots such as `x^2 - 6*x + 9 = 0` do not change sign. A scan therefore also looks for sampled points that are exact or near-zero, and for local minima or maxima where `abs(f(x))` becomes small. These are reported as touching-root candidates, with their residual, instead of silently saying no root was found.

Every reported root must pass a residual check. A sign change caused by a discontinuity, such as `1/(x-2) = 0`, may appear bracketed, but it is not a root if the final residual is not within tolerance. Such cases are rejected or reported as suspected discontinuities.

## DIDACTIC OUTPUT

With `--explain`, `solve` shows the path toward the answer rather than only the result. The trace is meant to be useful for learning, not just debugging implementation internals.

A bisection explanation should include:

- the transformed zero function, for example `f(x) = (x^2 - 2) - 0`
- the starting interval and endpoint values
- why the interval brackets a root, or why it does not
- each midpoint approximation, function value, and retained interval
- the stopping condition that ended the search
- the final root approximation and residual

A linear explanation shows the equation after moving everything to one side, the constant term being moved to the other side, division by the variable coefficient, and the final answer.

A quadratic explanation shows the standard coefficients, discriminant, quadratic formula, and, when the roots are simple rational-looking values, a factored form.

A higher-degree polynomial explanation shows rational roots found by factoring and any remaining lower-degree factor that must be solved separately.

A polynomial identity explanation says whether the transformed polynomial reduced to exact all-zero coefficients or only to floating-point coefficients that are zero within tolerance.

For intersections, `--explain` also shows that the two input expressions were converted into a single zero function, then reports both `x` and `y` for the intersection.

For touching-root candidates, `--explain` says that no sign change was found, shows the near-zero local extremum or exact sampled zero that triggered the candidate, and reports the residual clearly.

## OUTPUT

Normal text output is compact and stable. A single-root result includes at least:

```
x = 1.4142135623
residual = -0.0000000001
method = bisection
iterations = 34
```

Roots that are very close to integers are printed compactly, such as `x = 5`. When a non-integer root is close to a simple fraction with a small denominator, normal text output includes a didactic hint in parentheses, such as `x = 1.6666666667 (1 2/3)`.

For intersections, output includes the corresponding y-value:

```
x = 3
y = 9
residual = 0
method = bisection
iterations = 1
```

When `--quiet` is used, only the root value is printed. When `--all` is used, each result is printed as a separate solution block.

If no root is found, normal output says so and includes the searched range or interval. The exit status distinguishes success from failure:

- `0` at least one solution or candidate solution was found
- `1` no solution was found in the requested range
- `2` command-line usage or expression syntax error
- `3` numeric failure, such as division by zero during evaluation, overflow, or non-convergence after the iteration limit

## JSON OUTPUT

With `--json`, `solve` writes JSON Lines using the common envelope documented in `json-output`. Events include:

- `solve_result` for each root or intersection found
- `solve_candidate` for likely touching roots that meet the candidate rules but were not bracketed by a sign change
- `solve_identity` for supported polynomial identities; its data includes `exact:true` for integer-literal identities and `exact:false` for approximate decimal-coefficient identities
- `solve_summary` for method, status, and count information

A `solve_result` data object includes the variable name, root value, residual, method, iteration count, and, for intersections, the y-value. Diagnostics are written to stderr.

## LIMITATIONS

- no general symbolic algebra; `auto` can directly handle supported polynomial expressions through degree 16, including linear isolation, real quadratic roots, simple quadratic factoring explanations, rational-root factoring for higher degrees in that subset, and polynomial identities, but it does not factor degree-17 or higher polynomials, isolate variables in arbitrary expressions, or prove non-polynomial identities
- polynomial coefficient arithmetic uses `double`; integer-literal polynomial identities are exact within the integer range of `double`, while decimal-coefficient identities are reported as approximate rather than exact
- solves one variable at a time
- repeated polynomial roots in the supported symbolic subset are solved directly; other touching roots are only candidates unless the residual is within tolerance
- discontinuities can create false sign changes and must be reported carefully
- trigonometric and transcendental equations can have infinitely many roots; `solve` reports roots in the requested interval or scan range
- very flat functions may converge slowly or be missed by coarse scanning
- current evaluation uses binary floating-point internally, so it is not an arbitrary-precision decimal solver like `bc`
- `%` creates discontinuous sawtooth functions and is best treated as a numeric fallback operation rather than a symbolic polynomial operator

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

Show cubic rational-root factoring:

```
solve --explain --all 'x^3 - 6*x^2 + 11*x - 6 = 0'
```

Show higher-degree repeated-root factoring:

```
solve --explain --all '(x - 2)^5 = 0'
```

Show a polynomial identity:

```
solve --explain '(x + 1)^2 = x^2 + 2*x + 1'
```

## SEE ALSO

bc, expr, awk
