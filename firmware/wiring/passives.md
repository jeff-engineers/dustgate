# Reading the markings — resistors and ceramic caps

Every other page in this folder names passives by VALUE. This one says what that
value looks like on the actual part, so a build does not stall on a drawer of
unlabelled components.

**One page, linked from the others rather than copied into them** — the same rule
`canvas.html` follows. A colour code restated in five files is five places to get
it wrong.

## Ceramic capacitors

Through-hole ceramics carry a **three-digit EIA code in picofarads**: the first
two digits are the value, the third is how many zeros follow.

| Marking | pF | Is | Used for |
|---|---|---|---|
| `101` | 100 | 100 pF | |
| `102` | 1,000 | 1 nF | |
| `103` | 10,000 | 10 nF | |
| `104` | 100,000 | **100 nF = 0.1 µF** | the HF cap at every ADC pin |
| `105` | 1,000,000 | 1 µF | |
| `106` | 10,000,000 | **10 µF** | CT bias bulk |
| `107` | 100,000,000 | **100 µF** | CT bias bulk |

Divide pF by 1,000,000 for µF. Parts much above 1 µF are often printed with the
value outright (`10µF`) instead of a code. A trailing letter is tolerance
(`J` = ±5%, `K` = ±10%, `M` = ±20%), not part of the number.

**Ceramics are not polarised.** Any "long leg / `+`" instruction on these pages
applies only if you fit an ELECTROLYTIC, which has a stripe down its negative
side and must go the right way round.

**A big ceramic is smaller than it says.** Class II dielectrics (X5R, X7R) derate
hard with applied DC — a 100 µF X5R sitting at 1.65 V may deliver 40–60% of its
marking. Fine where the part is bulk decoupling, which is everywhere it appears
here; it matters only when a time constant is being computed from it (see
`ct-bench.md` on bias settling). Those dielectrics are also piezoelectric, so a
physically large one on a board bolted to a running machine is a microphone.

## Resistors

Four bands, the last being tolerance — gold is ±5% and is the common case:

| Value | Bands |
|---|---|
| **1 kΩ** | `brown black red gold` |
| **4.7 kΩ** | `yellow violet red gold` |
| **10 kΩ** | `brown black orange gold` |
| **33 kΩ** | `orange orange orange gold` |
| **1.0 MΩ** | `brown black green gold` |

Precision parts use **five** bands: a third significant digit moves in and the
multiplier shifts one place right.

| Value | Bands |
|---|---|
| **1 kΩ** | `brown black black brown brown` |
| **4.7 kΩ** | `yellow violet black brown brown` |
| **10 kΩ** | `brown black black red brown` |
| **33 kΩ** | `orange orange black red brown` |
| **1.0 MΩ** | `brown black black yellow brown` |

If the body is blue rather than beige, count five. When in doubt, meter it —
faster than arguing with a band that might be brown or might be red, and the one
failure this folder keeps recording is a rig built wrong and believed.
