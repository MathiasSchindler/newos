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

`solve` finds real solutions for common high-school "solve for x" problems. It is not a general symbolic algebra system, but `auto` has a bounded polynomial front end. Exact polynomial claims are tried first with rational coefficient arithmetic for supported rational-literal expressions through degree 8; the older floating-point polynomial recognizer still handles supported polynomial expressions through degree 16 as a fallback. Outside those subsets, `solve` treats an equation as a numeric function and searches for values of one variable that make the equation true.

For an input such as `x^2 - 2 = 0`, `solve` evaluates the left side minus the right side and looks for roots of that zero function. Intersections are the same problem: `x^2 = 2*x + 3` is solved as `x^2 - (2*x + 3) = 0`. With `--report-y`, `solve` also reports the corresponding y-value for the left side of the equation.

The tool is aimed at practical student workflows: checking algebra homework, exploring where graphs cross the x-axis, finding intersections between two curves, and seeing an approximate path toward the answer.

If no interval is supplied, `solve` scans a default visible school-math range rather than failing the most natural invocation. The default is `--scan -100:100:400`, followed by bisection on each bracketed sign change and special handling for likely touching roots.

## CURRENT CAPABILITIES

- one real variable, defaulting to `x`
- equation input as `left = right`, or zero-function input as a single expression where solutions satisfy `expression = 0`
- inequality input as `left < right`, `left <= right`, `left > right`, or `left >= right`, reported as interval notation
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
- exact rational polynomial solving through degree 8 for supported rational-literal expressions
- automatic direct solving for polynomial equations through degree 16 when the expression can be represented in the supported polynomial subset
- exact factoring explanations for quadratics with rational roots
- quadratic-formula solving for real quadratic roots
- rational-root factoring for higher-degree polynomials in the supported subset
- exact polynomial identity detection for rational-literal polynomial expressions, including finite decimal literals such as `0.1`
- exact polynomial derivatives and definite integrals for supported rational-literal polynomial expressions
- Simpson-rule numerical definite integration for non-polynomial expressions, with approximate labeling and an error estimate
- approximate identity reporting only when the exact rational front end cannot handle the expression and the floating-point fallback reduces the polynomial to 0 within tolerance
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
- `--diff[=N]` prints the Nth exact polynomial derivative; with an equation input, it solves the Nth derivative set equal to 0
- `--integrate A:B` computes the definite integral over `[A, B]`, exactly for supported rational polynomials and numerically otherwise
- `--json` writes machine-readable result and diagnostic events
- `--help` shows usage

`--scan` and `--lo`/`--hi` are mutually exclusive. `--scan` asks the tool to discover candidate intervals; `--lo` and `--hi` provide one explicit bracket. If neither form is supplied, `solve` behaves as if `--scan -100:100:400` had been supplied.

## METHODS

`bisection` is the primary numeric method. It requires an interval where the function changes sign, such as `--lo 1 --hi 2` for `x^2 - 2 = 0`. It is slower than Newton-style methods but predictable, robust, and easy to explain.

`auto` first tries to represent the transformed zero function as a polynomial with exact rational coefficients. This exact front end accepts the polynomial operators `+`, `-`, `*`, `/` by a nonzero constant, and integer powers, with integer and finite-decimal rational literals. If exact rational parsing, expansion, factoring, or bounded arithmetic fails, `auto` falls back to the existing floating-point polynomial recognizer and then to numeric scanning plus bisection.

Linear equations are solved by moving the constant term and dividing by the coefficient. Quadratics and factored higher-degree polynomials report every real root found by the symbolic path, even without `--all`. If the equation is outside the supported polynomial subsets, `auto` means "scan if needed, then use bisection on bracketed intervals." When a scan finds multiple roots and `--all` is not used, `solve` reports the root closest to zero. If an explicit bracket is supplied, `auto` is equivalent to `bisection`.

Quadratics are solved directly. When the exact rational discriminant is negative, `solve` reports that there are no real solutions. When the discriminant is a perfect square, roots are reported exactly as integers or fractions and `--explain` shows a factored form. When the discriminant is positive but not a rational square, roots are reported as decimal approximations with `status = approximate`. Higher-degree rational polynomials are factored only when exact rational roots are proven; any remaining quadratic factor is solved with the same exact discriminant check. Rational-literal polynomial equations that reduce to 0 are reported as exact identities, including decimal forms such as `(x + 0.1)^2 = x^2 + 0.2*x + 0.01`.

Inequalities form the same zero function and report solution intervals. Supported rational polynomials use exact roots and exact sign tests, so unbounded intervals such as `(-inf, -2)` and `[2, inf)` are true real intervals rather than clipped scan ranges. Non-polynomial inequalities use the numeric evaluator over the scan range and say that the answer is within that range.

`--diff` uses exact rational polynomial coefficients and the power rule. Without an equation, it prints the derivative polynomial. With an equation, it solves the derivative equal to 0, which is useful for extrema and inflection-point discussions. Non-polynomial differentiation is intentionally rejected.

`--integrate A:B` first tries exact rational polynomial integration by building an exact antiderivative and evaluating it at the bounds. If the integrand is outside the polynomial subset, it uses composite Simpson integration with one Richardson refinement and marks the result approximate. If evaluation fails inside the interval, for example across a pole, the integral is reported as improper and exits with numeric failure status.

Repeated roots such as `x^2 - 6*x + 9 = 0` do not change sign. A scan therefore also looks for sampled points that are exact or near-zero, and for local minima or maxima where `abs(f(x))` becomes small. These are reported as touching-root candidates, with their residual, instead of silently saying no root was found.

Every reported root must pass a residual check. A sign change caused by a discontinuity, such as `1/(x-2) = 0`, may appear bracketed, but it is not a root if the final residual is not within tolerance. Such cases are rejected or reported as suspected discontinuities, and `--explain` prints that warning when the discontinuity is encountered.

## DIDACTIC OUTPUT

With `--explain`, `solve` shows the path toward the answer rather than only the result. The trace is meant to be useful for learning, not just debugging implementation internals.

A bisection explanation should include:

- the transformed zero function, for example `f(x) = (x^2 - 2) - 0`
- the starting interval and endpoint values
- why the interval brackets a root, or why it does not
- each midpoint approximation, function value, and retained interval
- the stopping condition that ended the search
- the final root approximation and residual

Bisection-derived roots are decimal approximations and are marked with `status = approximate`. If scanning lands exactly on a root before interval refinement is needed, the method is reported as `exact-sample` with zero iterations.

A linear explanation shows the equation after moving everything to one side, the constant term being moved to the other side, division by the variable coefficient, and the final answer.

A quadratic explanation shows the standard coefficients, discriminant, quadratic formula, and, when exact rational roots are proven, a factored form.

A higher-degree polynomial explanation shows exact rational roots found by factoring and any remaining lower-degree factor that must be solved separately.

A polynomial identity explanation says whether the transformed polynomial reduced to exact all-zero rational coefficients or only to floating-point coefficients that are zero within tolerance after fallback.

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

Roots proven by exact rational polynomial solving are printed as exact integers or fractions, such as `x = 5` or `x = 5/3`. Numeric roots that are close to integers are printed compactly. When a numeric non-integer root is close to a simple fraction with a small denominator, normal text output includes a didactic hint in parentheses, such as `x = 1.6666666667 (1 2/3)`.

Inequality output uses interval notation, for example `solution = (-inf, -2) U (2, inf)` or `solution = all real x`. Definite integral output uses `integral = VALUE`, followed by either `method = exact-polynomial` or `method = simpson` with `status = approximate`.

For intersections, output includes the corresponding y-value:

```
x = 3
y = 9
residual = 0
method = bisection
iterations = 1
```

When `--quiet` is used, only the root value is printed. When `--all` is used, each scan result is printed as a separate solution block; symbolic polynomial methods print every root they determine directly.

If no root is found, normal output says whether this was an exact no-real-solution result, a miss in the default scan range, a miss in a requested scan range, or a miss in a requested interval. The exit status distinguishes success from failure:

- `0` at least one solution or candidate solution was found
- `1` no solution was found in the requested range
- `2` command-line usage or expression syntax error
- `3` numeric failure, such as division by zero during evaluation, overflow, or non-convergence after the iteration limit

## JSON OUTPUT

With `--json`, `solve` writes JSON Lines using the common envelope documented in `json-output`. Events include:

- `solve_result` for each root or intersection found
- `solve_candidate` for likely touching roots that meet the candidate rules but were not bracketed by a sign change
- `solve_identity` for supported polynomial identities; its data includes `exact:true` for exact rational-literal identities and `exact:false` for approximate floating-point fallback identities
- `solve_summary` for method, status, and count information

A `solve_result` data object includes the variable name, root value, residual, method, iteration count, and, for intersections, the y-value. Exact rational roots are emitted as fractions in the root string. Approximate roots include `exact:false`. Diagnostics are written to stderr.

## LIMITATIONS

- no general symbolic algebra; `auto` can directly handle supported exact rational polynomial expressions through degree 8 and supported floating-point polynomial expressions through degree 16, including linear isolation, real quadratic roots, simple quadratic factoring explanations, rational-root factoring for higher degrees in those subsets, and polynomial identities, but it does not factor degree-17 or higher polynomials, isolate variables in arbitrary expressions, or prove non-polynomial identities
- exact polynomial coefficient arithmetic is bounded; if rational numerators, denominators, common denominators, expansion products, divisor enumeration, or degree exceed the exact front-end limits, `solve` falls back to the floating-point polynomial or numeric path
- exact inequalities, derivatives, and polynomial integration are limited to the supported rational polynomial subset; non-polynomial inequalities and integrals use bounded numerical methods, while non-polynomial differentiation is not implemented
- numerical integration is composite Simpson integration, not a symbolic antiderivative engine, and improper integrals over detected discontinuities are rejected rather than assigned a finite number
- finite decimal literals in the exact polynomial front end are parsed as exact rationals from their source spelling, so `0.1` means exactly `1/10` in that path
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

Show an exact decimal-rational identity:

```
solve --explain '(x + 0.1)^2 = x^2 + 0.2*x + 0.01'
```

Solve an inequality exactly:

```
solve 'x^2 - 4 >= 0'
```

Print and solve a polynomial derivative:

```
solve --diff 'x^3 - 6*x^2 + 11*x - 6'
solve --diff 'x^3 - 6*x^2 + 11*x - 6 = 0'
```

Compute exact and numerical definite integrals:

```
solve --integrate 0:1 'x^2'
solve --integrate 0:pi 'sin(x)'
```

## SEE ALSO

bc, expr, awk
