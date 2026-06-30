# SOLVE

## NAME

solve - solve one-variable equations and intersections

## SYNOPSIS

```
solve [OPTIONS] 'EXPRESSION = EXPRESSION'
solve [OPTIONS] 'EXPRESSION'
solve [OPTIONS] --report-y 'Y1 = Y2'
solve [OPTIONS] --area A:B 'F' 'G'
solve [OPTIONS] --area 'F' 'G'
solve [OPTIONS] --area-quadrant I|II|III|IV 'F' 'G'
solve [OPTIONS] --volume A:B 'F'
solve [OPTIONS] --mean A:B 'F'
solve [OPTIONS] --average-rate A:B 'F'
solve [OPTIONS] --max --range A:B 'F'
solve [OPTIONS] --min --range A:B 'F'
solve [OPTIONS] --eval [--at x=A] 'F'
solve [OPTIONS] --subst x=EXPR 'F'
solve [OPTIONS] --fit-exp-asymptote A --points 'T1:Y1,T2:Y2'
```

## DESCRIPTION

`solve` finds real solutions for common high-school "solve for x" problems and, for a bare single expression, prints a compact curve overview by default. It is not a general symbolic algebra system, but `auto` has a bounded polynomial front end. Exact polynomial claims are tried first with rational coefficient arithmetic for supported rational-literal expressions through degree 8; the older floating-point polynomial recognizer still handles supported polynomial expressions through degree 16 as a fallback. Outside those subsets, `solve` treats an equation as a numeric function and searches for values of one variable that make the equation true.

For an input such as `x^2 - 2 = 0`, `solve` evaluates the left side minus the right side and looks for roots of that zero function. Intersections are the same problem: `x^2 = 2*x + 3` is solved as `x^2 - (2*x + 3) = 0`. With `--report-y`, `solve` also reports the corresponding y-value for the left side of the equation.

For a bare expression such as `x^3 - 3*x`, the default is an overview report: domain, symmetry, zeros, extrema, inflection points, monotonicity, curvature, and end behavior when available. To ask only for roots of a zero function, write the equation explicitly, for example `x^3 - 3*x = 0`, or supply an explicit solving option such as `--scan` or `--quiet`.

The tool is aimed at practical student workflows: checking algebra homework, exploring where graphs cross the x-axis, finding intersections between two curves, doing Abitur-style curve discussion, and seeing an approximate path toward the answer.

If no interval is supplied, `solve` scans a default visible school-math range rather than failing the most natural invocation. The default is `--scan -100:100:400`, followed by bisection on each bracketed sign change and special handling for likely touching roots.

## CURRENT CAPABILITIES

- one real variable, defaulting to `x`
- equation input as `left = right`, or zero-function solving with an explicit solving option; a bare single expression defaults to overview mode
- inequality input as `left < right`, `left <= right`, `left > right`, or `left >= right`, reported as interval notation
- intersection solving through the same equation form, with `--report-y` for y-value output
- arithmetic operators `+`, `-`, `*`, `/`, `%`, and `^`
- unary `+` and unary `-`
- parentheses for grouping
- decimal numeric constants
- predefined constants `pi` and `e`
- built-in functions such as `sqrt(x)`, `abs(x)`, `min(x, y)`, and `max(x, y)`
- math-library functions such as `sin(x)`, `cos(x)`, `tan(x)`, `asin(x)`, `acos(x)`, `atan(x)`, `sinh(x)`, `cosh(x)`, `tanh(x)`, `log(x)`, `exp(x)`, and rounding helpers `floor(x)`, `ceil(x)`, `round(x)`
- interval solving with `--lo` and `--hi`
- automatic interval scanning to discover sign changes over a range
- exact rational polynomial solving through degree 8 for supported rational-literal expressions
- automatic direct solving for polynomial equations through degree 16 when the expression can be represented in the supported polynomial subset
- exact factoring explanations for quadratics with rational roots
- quadratic-formula solving for real quadratic roots
- rational-root factoring for higher-degree polynomials in the supported subset
- exact polynomial identity detection for rational-literal polynomial expressions, including finite decimal literals such as `0.1`
- exact polynomial derivatives and definite integrals for supported rational-literal polynomial expressions
- bounded symbolic derivatives for common non-polynomial expressions using sum, product, quotient, power, and chain rules, with simple numeric constant folding
- simplified symbolic derivatives for common exponential-polynomial products of the form `exp(linear)*polynomial`, with exact polynomial critical-point factors
- parameter names declared with `--param NAME` for symbolic differentiation with respect to the selected variable only, or bound numerically with `--param NAME=VALUE` for numeric evaluation and differentiation paths
- direct expression evaluation with `--eval`, point evaluation with `--at`, and textual identifier substitution with `--subst`
- exact polynomial antiderivatives, plus a small symbolic antiderivative table for `sin(a*x)`, `cos(a*x)`, `exp(a*x)`, and `1/x`
- exact polynomial monotonicity, curvature, tangent and normal lines, end behavior, and curve-discussion summaries
- average rates and numeric extrema over finite ranges, with exploratory finite windows for infinite range endpoints
- exact polynomial area between two curves, with numeric omitted-bound discovery from an explicit `--scan` for non-polynomial curves; rotation volume around the x-axis; and mean value over an interval
- quadrant-lobe area selection for exam-style enclosed regions when polynomial intersections can be found
- a two-point exponential-asymptote fit for models of the form `a + b*exp(-c*x)`
- simple numeric limit checks and rational-polynomial asymptote detection, including polynomial-plus-quotient forms such as `1 + 2/(x-3)`
- Simpson-rule numerical definite integration for non-polynomial expressions, with approximate labeling and an error estimate; rational poles inside the interval are classified as divergent improper integrals
- approximate identity reporting only when the exact rational front end cannot handle the expression and the floating-point fallback reduces the polynomial to 0 within tolerance
- multiple-root reporting when scanning finds more than one candidate interval
- likely touching-root reporting for repeated roots that do not change sign, including adaptive refinement of near-zero sampled non-polynomial candidates
- default overview output for bare single expressions, plus student-facing didactic output with method, assumptions, exactness, and verification; `--explain=trace` adds scan and iteration details
- ANSI styling for interactive stdout: important answer lines are emphasized, warnings are yellow, and problems are red; redirected output and JSON stay plain
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
- `--explain` prints a student-facing worked solution for the selected solving, inequality, calculus, or analysis mode
- `--explain=trace` prints the technical algorithm trace, including scan, bisection, and discontinuity diagnostics
- `--quiet` prints only the final answer
- `--param NAME` declares `NAME` as a symbolic parameter rather than the solving variable; `--param NAME=VALUE` additionally binds a numeric value for evaluation, differentiation, numeric bounds, and numeric integration paths
- `--diff[=N]` prints the Nth derivative; exact rational-polynomial derivatives are preferred, then bounded symbolic differentiation is tried for common expression forms; with an equation input, it solves the Nth derivative set equal to 0
- `--integrate A:B` computes the definite integral over `[A, B]`, exactly for supported rational polynomials and numerically otherwise; bound expressions may reference numeric parameters declared with `--param NAME=VALUE`
- `--antiderivative` prints an exact polynomial antiderivative plus `C`, or a symbolic-table antiderivative for a few simple school functions
- `--monotonicity` reports increasing and decreasing intervals; supported `exp(linear)*polynomial` expressions use exact polynomial derivative factors for interval signs
- `--curvature` reports left-curved and right-curved intervals; supported `exp(linear)*polynomial` expressions use exact polynomial second-derivative factors for interval signs
- `--tangent A` prints the tangent line at `x = A`
- `--normal A` prints the normal line at `x = A`
- `--end-behavior` prints polynomial limits as `x` approaches positive and negative infinity
- `--discuss` prints a compact curve discussion: domain, symmetry, zeros, extrema, inflection points, monotonicity, curvature, and end behavior when available
- `--area A:B F G` computes area between two curves on `[A, B]`; if `A:B` is omitted, `--area F G` uses the leftmost and rightmost exact polynomial intersections when they can be proven, or roots found by an explicit `--scan` range for numeric omitted-bound area
- `--area-quadrant I|II|III|IV F G` chooses the enclosed lobe whose midpoint lies in the requested quadrant and computes its area
- `--volume A:B F` computes the volume of rotation around the x-axis as `pi*(integral f(x)^2 dx)`, exactly for supported rational polynomials and numerically otherwise
- `--mean A:B F` computes the mean value over `[A, B]`, exactly for supported rational polynomials and numerically otherwise
- `--average-rate A:B F` computes `(F(B)-F(A))/(B-A)`
- `--max --range A:B F` and `--min --range A:B F` compare endpoints and numeric critical points over the range; `inf` endpoints are sampled over an exploratory finite window
- `--eval F` evaluates an expression at the current variable value 0 unless `--at` is supplied; if `--at x=A` is symbolic, it falls back to simplified symbolic substitution
- `--at x=A` supplies a point for `--eval`
- `--subst NAME=EXPR F` replaces identifier occurrences textually, then applies bounded expression simplification before printing the result
- `--fit-exp-asymptote A --points T1:Y1,T2:Y2` fits `A + b*exp(-c*x)` through two points
- `--limit x->A` samples a two-sided limit, with exact rational simplification for supported rational-polynomial quotients
- `--asymptotes` reports vertical, horizontal, or oblique asymptotes for supported rational-polynomial quotients and simple polynomial-plus-quotient forms
- `--json` writes machine-readable result and diagnostic events
- `--help` shows usage

`--scan` and `--lo`/`--hi` are mutually exclusive. `--scan` asks the tool to discover candidate intervals; `--lo` and `--hi` provide one explicit bracket. If neither form is supplied for equation or inequality solving, `solve` behaves as if `--scan -100:100:400` had been supplied. For a bare single expression without a solving option, the default is overview mode instead of root-only solving.

## METHODS

`bisection` is the primary numeric method. It requires an interval where the function changes sign, such as `--lo 1 --hi 2` for `x^2 - 2 = 0`. It is slower than Newton-style methods but predictable, robust, and easy to explain.

`auto` first tries to represent the transformed zero function as a polynomial with exact rational coefficients. This exact front end accepts the polynomial operators `+`, `-`, `*`, `/` by a nonzero constant, and integer powers, with integer and finite-decimal rational literals. If exact rational parsing, expansion, factoring, or bounded arithmetic fails, `auto` falls back to the existing floating-point polynomial recognizer and then to numeric scanning plus bisection.

Linear equations are solved by moving the constant term and dividing by the coefficient. Quadratics and factored higher-degree polynomials report every real root found by the symbolic path, even without `--all`. If the equation is outside the supported polynomial subsets, `auto` means "scan if needed, then use bisection on bracketed intervals." When a scan finds multiple roots and `--all` is not used, `solve` reports the root closest to zero. If an explicit bracket is supplied, `auto` is equivalent to `bisection`. Bare single-expression input without a solving option is routed to the same curve-discussion machinery as `--discuss`, because an overview is usually more useful than one arbitrary zero.

Quadratics are solved directly. When the exact rational discriminant is negative, `solve` reports that there are no real solutions. When the discriminant is a perfect square, roots are reported exactly as integers or fractions and `--explain` shows a factored form. When the discriminant is positive but not a rational square, roots are reported as decimal approximations with `status = approximate`. Higher-degree rational polynomials are factored only when exact rational roots are proven; any remaining quadratic factor is solved with the same exact discriminant check. Rational-literal polynomial equations that reduce to 0 are reported as exact identities, including decimal forms such as `(x + 0.1)^2 = x^2 + 0.2*x + 0.01`.

Inequalities form the same zero function and report solution intervals. Supported rational polynomials use exact roots and exact sign tests, so unbounded intervals such as `(-inf, -2)` and `[2, inf)` are true real intervals rather than clipped scan ranges. Non-polynomial inequalities use the numeric evaluator over the scan range and say that the answer is within that range.

`--diff` uses exact rational polynomial coefficients and the power rule when possible. Numeric parameters declared with `--param NAME=VALUE` are substituted before derivative parsing, while unbound parameters declared with `--param NAME` are treated as symbolic constants. If the expression is not in the polynomial subset, it first recognizes common products of an exact rational polynomial with `exp(linear)`, such as `x^2*exp(-x)` or `exp(-x)*(x^2-3*x)`. For those expressions, the nonzero exponential factor is kept separate and the derivative factor is computed with exact rational polynomial arithmetic. It then tries a bounded symbolic differentiator for the existing expression language: sums, differences, products, quotients, constant powers, `sin`, `cos`, `tan`, `asin`, `acos`, `sinh`, `cosh`, `tanh`, `exp`, `log`/`ln`, `sqrt`, `atan`, and the corresponding chain rule. Without an equation, `--diff` prints the derivative expression. With an equation, it solves the derivative equal to 0, which is useful for extrema and inflection-point discussions.

`--integrate A:B` first tries exact rational polynomial integration by building an exact antiderivative and evaluating it at literal rational bounds. Bound expressions may reference numeric parameters declared with `--param NAME=VALUE`; those bounds are evaluated numerically, so the Simpson path is used unless the exact polynomial path can still prove the complete integral from literal rational data. If the integrand is outside the polynomial subset, or exact rational evaluation overflows because a decimal bound expands to a very large rational, it uses composite Simpson integration with one Richardson refinement and marks the result approximate. If evaluation fails inside the interval, for example across a pole, the integral is reported as improper and exits with numeric failure status. Supported rational-polynomial quotients are additionally checked for denominator roots inside the interval; a detected pole is classified as a divergent improper integral.

The analysis modes are additive front ends over the same evaluator, exact rational polynomial layer, root finder, and interval sign analysis. `--antiderivative`, `--monotonicity`, `--curvature`, `--tangent`, `--normal`, `--end-behavior`, `--area`, `--volume`, and `--mean` prefer exact rational polynomial arithmetic. When a polynomial antiderivative is not available, `--antiderivative` has a deliberately small table for simple school functions such as `sin(x)`, `cos(2*x)`, `exp(-0.5*x)`, and `1/x`. Recognized `exp(linear)*polynomial` expressions reuse exact polynomial derivative factors for critical x-values, monotonicity intervals, curvature intervals, tangent and normal slopes, and curve discussion; the corresponding function values remain approximate when they involve `exp`. Other non-polynomial monotonicity, curvature, tangent, normal, discussion, area, volume, mean, and limit work use numerical sampling and label such output approximate.

`--eval`, `--at`, and `--subst` are convenience modes for Abitur-style intermediate work. `--eval --at x=A F` evaluates a numeric function value directly; if `A` is a symbolic identifier or expression that cannot be evaluated numerically, it substitutes and simplifies instead, printing `value expression = ...` and `method = symbolic-substitution`. Numeric parameters declared with `--param NAME=VALUE` are substituted into evaluator input before parsing, so `--param k=2 --eval --at x=3 'k*x + 1'` reports `7`. `--subst NAME=EXPR F` performs identifier substitution and then a bounded simplification pass, so common zero differences such as `(k-k)` collapse in expressions such as `f_k(k)`, and literal arithmetic such as `2^2 + 3*2` folds to `10`. `--average-rate A:B F` evaluates the secant slope `(F(B)-F(A))/(B-A)`. `--max` and `--min` over `--range A:B` compare endpoints and critical points. For `exp(linear)*polynomial` expressions with a decaying exponential over a one-sided infinite exam-style range, the critical x-values are found from an exact polynomial derivative factor, while the function values remain approximate because they involve `exp`. Other infinite endpoints are explored over a finite window, so the result is approximate. `--fit-exp-asymptote A --points T1:Y1,T2:Y2` fits `A + b*exp(-c*x)` by eliminating `b` from the two shifted point values.

`--area-quadrant` is an exam-style helper for enclosed lobes. It finds consecutive polynomial intersections, selects the interval whose midpoint lies in the requested quadrant, and computes the lobe area. If the selected endpoints are exact rational roots, the area can be exact; otherwise it falls back to Simpson integration and labels the result approximate. For polynomial lobes whose numeric Simpson result is very close to a simple rational, it also prints a separate `rational area hint = ...` line; this is a didactic hint, not an exactness claim. Plain `--area F G` without explicit bounds first tries exact polynomial intersections; if that fails and an explicit `--scan` range is present, it reuses the scan root finder to choose the leftmost and rightmost numeric intersections, then integrates `|F-G|` numerically between those bounds.

`--discuss` combines the primitive analysis results. For supported polynomials it reports exact zeros, extrema classified by derivative sign change, saddle points where the first derivative does not change sign, inflection points from the second derivative, monotonicity and curvature intervals, and end behavior. For recognized `exp(linear)*polynomial` expressions it reports the full real domain, exact factor roots for zeros and derivative sign intervals, approximate point values, and end-behavior hints. Other non-polynomial expressions report the sampled window, numeric zeros, critical points, inflection points, monotonicity and curvature intervals, and simple end-behavior or horizontal-asymptote hints where sampling supports them. Those fallback results are approximate and bounded by the scan window.

`--limit x->A` evaluates a two-sided limit near `A` by sampling from the left and right. Removable holes in simple rational expressions are detected numerically by the agreement of nearby left and right values. Opposite-sided blow-up is reported as no two-sided limit with a pole rather than as a finite number. Finite jumps, such as `abs(x)/x` at `0`, are reported as no two-sided limit with the sampled left and right values.

`--asymptotes` recognizes rational-polynomial quotients such as `(x^2 + 1)/(x - 1)`. It also normalizes simple sums of a polynomial and one rational quotient, such as `1 + 2/(x-3)` or `2/(x-3) + 1`, into a single quotient before analysis. Real denominator zeros that are not canceled are vertical asymptotes, and polynomial division of the numerator by the denominator gives a horizontal or oblique asymptote when the quotient degree is 0 or 1.

Repeated roots such as `x^2 - 6*x + 9 = 0` do not change sign. A scan therefore also looks for sampled points that are exact or near-zero, and for local minima or maxima where `abs(f(x))` becomes small. For near-zero sampled non-polynomial candidates, it adaptively refines the neighboring interval before reporting the candidate. These are reported as touching-root candidates, with their residual, instead of silently saying no root was found.

Every reported root must pass a residual check. A sign change caused by a discontinuity, such as `1/(x-2) = 0`, may appear bracketed, but it is not a root if the final residual is not within tolerance. Such cases are rejected or reported as suspected discontinuities, and `--explain=trace` prints that warning when the discontinuity is encountered.

## DIDACTIC OUTPUT

With `--explain`, `solve` shows a classroom-style worked solution rather than only the result. It is aimed at students and teachers: output starts with `worked solution`, shows the given task, variable, goal, rewrite into a working function, basic domain assumptions such as square-root, logarithm, and denominator restrictions, the method choice, the final answer, an exactness or approximation statement, and a substitution check.

Use `--explain=trace` for the developer or advanced-student view. Trace mode preserves the algorithmic details: transformed zero function, scan window, bisection midpoint iterations, suspected discontinuity warnings, and low-level method diagnostics. It intentionally does not print the student `worked solution` header.

For every supported mode, explanation output names the exact or numeric method being used, prints the decisive intermediate values, states the mathematical rule behind the classification, and marks the honesty boundary between exact proof and numeric sampling.

A student-facing bisection explanation includes the rewrite, method, final approximation, residual, exactness statement, and substitution check. A trace bisection explanation additionally includes:

- the transformed zero function, for example `f(x) = (x^2 - 2) - 0`
- the starting interval and endpoint values
- why the interval brackets a root, or why it does not
- each midpoint approximation, function value, and retained interval
- the stopping condition that ended the search

Bisection-derived roots are decimal approximations and are marked with `status = approximate`. If scanning lands exactly on a root before interval refinement is needed, the method is reported as `exact-sample` with zero iterations.

A linear explanation shows the equation after moving everything to one side, the constant term being moved to the other side, division by the variable coefficient, and the final answer.

A quadratic explanation shows the standard coefficients, discriminant, quadratic formula, and, when exact rational roots are proven, a factored form.

A higher-degree polynomial explanation shows exact rational roots found by factoring and any remaining lower-degree factor that must be solved separately.

A polynomial identity explanation says whether the transformed polynomial reduced to exact all-zero rational coefficients or only to floating-point coefficients that are zero within tolerance after fallback.

For intersections, `--explain` also shows that the two input expressions were converted into a single zero function, then reports both `x` and `y` for the intersection.

For touching-root candidates, `--explain` reports the result and residual in student language. `--explain=trace` adds the scan detail that identified the near-zero local extremum or exact sampled zero.

For exact polynomial analysis modes, `--explain` prints the relevant derivative, antiderivative, sign rule, point evaluation, leading-term rule, or polynomial division step. For example, monotonicity explains that roots of `f'` split the real line and that the sign of `f'` decides increasing or decreasing intervals; curvature does the same with `f''`; tangent and normal output show `f(a)`, `f'(a)`, the slope rule, and the intercept construction. For recognized `exp(linear)*polynomial` monotonicity, curvature, and discussion, explanation output prints the exact polynomial derivative factor and states that the exponential factor is positive, so it cannot change the sign intervals.

For `--subst` and symbolic `--eval --at`, explanation output shows the original expression, the replacement assignment, the expression immediately after identifier replacement, and the simplified expression. When applicable it also names the bounded simplification rules used, such as identical-term cancellation `k - k = 0`, zero products, zero powers, and neutral-term removal.

For `--discuss`, explanation output is a compact curve-discussion certificate. Polynomial input shows the domain and symmetry tests, zeros, first and second derivatives, extremum and inflection rules, interval sign analysis, and leading-term end behavior. Recognized `exp(linear)*polynomial` input shows the exact zero, first-derivative, and second-derivative factors before reporting approximate values. Other non-polynomial input states the scan window, explains that zeros come from scan plus bisection, critical and inflection points from finite-difference sign changes, and end-behavior hints from far samples.

For `--area`, explanation output constructs `h(x) = f(x) - g(x)`, states that area is `integral |h(x)| dx`, and prints the cut roots used to split positive and negative lobes when the exact polynomial path is available. Numeric area explains that Simpson integration samples `|f(x)-g(x)|` directly. If the area is a polynomial lobe with non-rational numeric endpoints and the Simpson result is close to a simple rational, normal output may include a separate rational area hint.

For `--integrate`, `--volume`, and `--mean`, explanation output prints the formula being evaluated. Exact polynomial paths show the exact integrand and antiderivative or integral value; numeric paths show the Simpson subdivision counts and estimates. Volume explains the `pi * integral f(x)^2 dx` disc formula, and mean value explains division by interval width.

For `--limit`, explanation output prints the target point, final left and right samples, and the classification rule: matching one-sided values indicate a two-sided limit, divergent magnitude indicates a pole, and finite mismatch indicates a jump. For `--asymptotes`, explanation output shows numerator, denominator, denominator roots, numerator values at those roots, polynomial quotient, remainder, and the division form that justifies horizontal or oblique asymptotes.

## OUTPUT

Normal text output is compact and stable. Interactive stdout uses ANSI styling when the terminal and environment allow it: answer-bearing lines are bold bright white, warnings are yellow, and problem lines are red. Redirected output, `NO_COLOR`, and JSON output remain unstyled. A single-root result includes at least:

```
x = 1.4142135623
residual = -0.0000000001
method = bisection
iterations = 34
```

Roots proven by exact rational polynomial solving are printed as exact integers or fractions, such as `x = 5` or `x = 5/3`. Numeric roots that are close to integers are printed compactly. When a numeric non-integer root is close to a simple fraction with a small denominator, normal text output includes a didactic hint in parentheses, such as `x = 1.6666666667 (1 2/3)`.

Inequality output uses interval notation, for example `solution = (-inf, -2) U (2, inf)` or `solution = all real x`. Definite integral output uses `integral = VALUE`, followed by either `method = exact-polynomial` or `method = simpson` with `status = approximate`. Analysis output uses labels such as `maximum`, `minimum`, `saddle`, `inflection`, `increasing`, `right-curved`, `tangent`, `area`, and `volume`, followed by `method = exact-polynomial` when the result is exact or `status = approximate` when it depends on numeric sampling. Recognized `exp(linear)*polynomial` discussion uses `method = exact-exp-polynomial-critical-points` and `status = approximate-values` because x-values come from exact factors but y-values are evaluated numerically. Numeric curve discussion prints `sample window: [LO, HI]` instead of claiming a complete symbolic domain.

For intersections, output includes the corresponding y-value:

```
x = -1
y = 1
residual = 0.0000000000
method = factoring
iterations = 0

x = 3
y = 9
residual = 0.0000000000
method = factoring
iterations = 0
```

When `--quiet` is used, only the root value is printed. When `--all` is used, each scan result is printed as a separate solution block; symbolic polynomial methods print every root they determine directly.

If no root is found, normal output says whether this was an exact no-real-solution result, a miss in the default scan range, a miss in a requested scan range, or a miss in a requested interval. The exit status distinguishes success from failure:

- `0` at least one solution or candidate solution was found
- `1` no solution was found in the requested range
- `2` command-line usage or expression syntax error
- `3` numeric failure, such as division by zero during evaluation, overflow, or non-convergence after the iteration limit

## JSON Output

With `--json`, `solve` writes JSON Lines using the common envelope documented in `json-output`. Every mode emits JSON only; no plain text leaks onto stdout. Events include:

- `solve_result` for each root or intersection found
- `solve_candidate` for likely touching roots that meet the candidate rules but were not bracketed by a sign change
- `solve_identity` for supported polynomial identities; its data includes `exact:true` for exact rational-literal identities and `exact:false` for approximate floating-point fallback identities
- `solve_summary` for method, status, and count information
- `solve_value` for single-field analysis results such as `--eval`, `--diff`, `--integrate`, `--average-rate`, and `--subst`, with `data` of the form `{"key":...,"value":...}`
- `solve_output` for the remaining analysis, inequality, calculus, and curve-discussion lines, with `data` of the form `{"text":...}`; one event is emitted per logical output line

A `solve_result` data object includes the variable name, root value, residual, method, iteration count, and, for intersections, the y-value. Exact rational roots are emitted as fractions in the root string. Approximate roots include `exact:false`. Diagnostics are written to stderr.

## LIMITATIONS

- no general symbolic algebra; `auto` can directly handle supported exact rational polynomial expressions through degree 8 and supported floating-point polynomial expressions through degree 16, including linear isolation, real quadratic roots, simple quadratic factoring explanations, rational-root factoring for higher degrees in those subsets, and polynomial identities, but it does not factor degree-17 or higher polynomials, isolate variables in arbitrary expressions, or prove non-polynomial identities
- exact polynomial coefficient arithmetic is bounded; if rational numerators, denominators, common denominators, expansion products, divisor enumeration, or degree exceed the exact front-end limits, `solve` falls back to the floating-point polynomial or numeric path
- exact inequalities, derivatives, polynomial integration, and most exact analysis results are limited to the supported rational polynomial subset; recognized `exp(linear)*polynomial` expressions additionally provide exact derivative-factor x-values and tangent/normal slopes, while non-polynomial inequalities, integrals, volume, mean, and unrecognized discussion use bounded numerical methods
- symbolic non-polynomial derivatives are intentionally expression-level output, not a general simplifier; common zero/one factors, numeric literal arithmetic, and `exp(linear)*polynomial` products are simplified, but other results may still contain nested parentheses or unsimplified algebraic structure
- `--param NAME=VALUE` binds numeric parameters for evaluator-based modes, differentiation, numeric bounds, and numeric integration fallback; exact rational polynomial solving and exact polynomial integration still require literal rational expressions in the parsed expression or bounds
- `--subst` applies bounded expression simplification after substitution, including identical-term cancellation and numeric constant folding, but it is not a general algebraic simplifier or identity prover
- `--max` and `--min` over infinite ranges can use exact critical-point factors for recognized decaying `exp(linear)*polynomial` expressions, but otherwise use finite exploratory windows and cannot prove global extrema in general
- `--area F G` without explicit bounds requires at least two exact polynomial intersections, or an explicit `--scan` range that finds at least two numeric intersections; numeric omitted-bound areas are approximate and bounded by that scan range
- `--asymptotes` is limited to rational-polynomial quotients and simple sums of one polynomial with one quotient; general algebraic normalization is not implemented
- numerical integration is composite Simpson integration, not a symbolic antiderivative engine; rational poles inside an interval are classified as divergent, and other improper integrals over detected discontinuities are rejected rather than assigned a finite number
- finite decimal literals in the exact polynomial front end are parsed as exact rationals from their source spelling, so `0.1` means exactly `1/10` in that path
- exact rational and floating-point polynomial roots are reported even when they fall outside the default scan window, so a perfect square such as `x^2 = 40000` reports `x = +/-200`; an explicit `--scan LO:HI` still clips reported roots to that window
- the unary functions `sqrt`, `abs`, `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `sinh`, `cosh`, `tanh`, `log`/`ln`, `exp`, `floor`, `ceil`, `round` and the binary `min`/`max` are built in; `tan` near its asymptotes can present sign changes that are screened by the residual check, and `floor`, `ceil`, `round` are evaluated but have no symbolic derivative
- solves one variable at a time
- repeated polynomial roots in the supported symbolic subset are solved directly; other touching roots are refined from near-zero sampled candidates and reported only when the residual is within tolerance
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

Show a student-facing bisection solution:

```
solve --explain --method bisection --lo 1 --hi 2 'x^2 - 2 = 0'
```

Show the technical bisection trace:

```
solve --explain=trace --method bisection --lo 1 --hi 2 'x^2 - 2 = 0'
```

Show a factored quadratic path:

```
solve --explain --all 'x^2 - 5*x + 6 = 0'
```

Show cubic rational-root factoring:

```
solve --explain --all 'x^3 - 6*x^2 + 11*x - 6 = 0'
```

Print the default overview for a function:

```
solve 'x^3 - 3*x'
solve --explain 'x^3 - 3*x'
solve --json 'x^3 - 3*x'
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

Print an exact antiderivative and an exact tangent line:

```
solve --antiderivative 'x^2'
solve --tangent 2 'x^2'
```

Discuss a polynomial curve:

```
solve --discuss 'x^3 - 3*x'
```

Compute area, rotation volume, and mean value:

```
solve --area -1:1 'x^3 - x' '0'
solve --area '2*x' 'x^2'
solve --volume 0:1 'x'
solve --mean 0:2 '2*x'
```

Check a removable limit and rational asymptotes:

```
solve --limit 'x->2' '(x^2 - 4)/(x - 2)'
solve --asymptotes '(x^2 + 1)/(x - 1)'
```

Show a curve-discussion certificate:

```
solve --explain --discuss 'x^3 - 3*x'
```

## SEE ALSO

bc, expr, awk
