# Paper and mathematics

Two independent LaTeX documents:

1. [Mathematical Foundations and Derivations](math_foundations.tex): the learning
   route from derivatives and Jacobians through Nanson's formula, stress,
   weak forms, MPM, CK weights, affine transfers, MLS and sand plasticity.
2. [Paper-to-Code Map](paper_code_map.tex): connects paper sections and equations
   to the current shader/C++ functions and identifies implementation differences.

Start with the foundations if an equation feels unfamiliar. Each section lists
its prerequisites and includes a derivation or worked check. The opening map
shows dependencies; the final trace connects the foundations to one timestep.
Numbered equations in the learning guide are local to that document. References
such as “paper Eq. (2)” explicitly refer to the CK-MPM paper.

The documents distinguish standard mathematics, paper results, implementation
choices and unresolved convention checks. In particular, the MLS stress/moment
matrix ordering and printed paper Eq. (17) are not assumed equivalent to the
current author-derived shader expressions.

## Build

With a LaTeX distribution providing `pdflatex`, `amsmath`, `amssymb`, `geometry`,
`booktabs`, `longtable` and `hyperref`:

```sh
cd docs/paper_math                 # From vulkan-starter
pdflatex -halt-on-error math_foundations.tex
pdflatex -halt-on-error math_foundations.tex
pdflatex -halt-on-error paper_code_map.tex
pdflatex -halt-on-error paper_code_map.tex
```

Two passes resolve contents, cross-references and table widths. PDFs appear
beside the sources; generated files are ignored by Git. Keep both PDFs in this
folder for their companion/source links to resolve. PDF viewers may require
approval before opening relative source-file links.

The local paper is `../../../CK-MPM Paper.pdf`; source links use paths relative
to the Vulkan project root. The neighboring CK-MPM checkout supplies the author
source links. Bibliographies include primary papers and continuum-mechanics
teaching references.

No LaTeX compiler is installed in the current workspace. Source structure and
links have been checked, but PDF compilation and layout remain unverified.
