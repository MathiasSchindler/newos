# Nemotron ASR auf Snapdragon X: Implementierungsplan

Stand: 2026-10-14. Dies ist ein Plan, kein Nachweis eines bereits
funktionierenden Nemotron-QNN-Exports.

## Ziel und Grenzen

Ein freestanding Windows-ARM64-Programm fuer Snapdragon X soll
`nvidia/nemotron-3.5-asr-streaming-0.6b` lokal und inkrementell ausfuehren.
Eigener Code und wiederverwendete newos-Komponenten werden statisch gelinkt,
ohne libc, CRT, C++-Runtime oder externe Inferenzbibliothek.
Moeglichst viel Modellrechnung soll auf der Qualcomm-HTP-NPU stattfinden.

Langfristiges Ziel: maximale nutzbare NPU-Beschleunigung bei minimaler
Host-CPU-Arbeit und moeglichst geringer Latenz. Der korrekte FP32/HVX-Pfad
ist eine Basis, keine Behauptung voller HMX-/Tensor-Engine-Nutzung.
Spaetere Tensor-Engine-Offloads muessen die unveraenderten numerischen Gates
erfuellen. CPU-Referenz, Export, Signing und Audits sind Entwicklungskosten,
nicht CPU-Inferenz im ausgelieferten Programm. Warmen Host-CPU-Verbrauch
separat von Wandzeit messen; blockierendes Warten auf DSP ist keine CPU-Rechnung.

Backend-Entscheidung am 2026-10-14: ausschliesslich dieses Windows-System,
kein WSL und kein Linux-Prepare-Host. Die gepinnte Windows-QNN-Runtime kann
eigene Host-Op-Packages nicht laden. Nach expliziter Freigabe wird fuer
Nemotron ein eigener signierter FastRPC-FP32-Graph-Executor auf Hexagon/HVX
implementiert. Die M0-Referenz, numerischen Toleranzen und Latenzziele bleiben
unveraendert. QNN-Ergebnisse bleiben als Vergleich und dokumentierter
Windows-SDK-Blocker erhalten; FastRPC-Ergebnisse werden nicht als bestandene
QNN-Graph-Abnahme bezeichnet.

- Keine Python-, PyTorch-, NeMo-, ONNX-Runtime-, CUDA- oder DirectML-Abhaengigkeit
  im ausgelieferten Programm.
- Windows-System-APIs bleiben fuer Speicher, Dateien, Zeitmessung,
  DLL-Laden und spaeter Mikrofoneingabe notwendig. "Statisch" bedeutet hier
  nicht ein Windows-Programm ohne System-DLLs.
- Die einzige externe Laufzeitausnahme sind die notwendigen Qualcomm-NPU-DLLs
  samt Treiber/Firmware. Deren eigene transitive DLL-Abhaengigkeiten werden
  inventarisiert; der eigene Code darf keine CRT-Abhaengigkeit erhalten.
- Fuer den freigegebenen FP32-Pfad wird eigener freestanding Code statisch
  in die ARM64-EXE und ein separates signiertes V73-DSP-Modul gelinkt.
  Das DSP-Modul ist ein eigenes Programmartefakt, keine fremde Inferenzbibliothek.
  Der OEM-FastRPC-Transport gehoert zur sichtbaren Qualcomm-Laufzeitausnahme.
  Vorhandene Entwicklungs-Signing-Identitaet verwenden; keine automatischen
  Zertifikats-, Treiber-, Boot- oder Trust-Aenderungen.
- Modellgewichte, Tokenizer und kompilierte QNN-Kontexte sind lokale
  Datenartefakte, keine zusaetzlichen Laufzeitbibliotheken.
- Entwicklungsseitig sind Python, NeMo und Qualcomm-Konverter erlaubt.
  Sie gehoeren nicht zur Distribution. Export kann auf einem anderen Rechner
  stattfinden, Ausfuehrung und Abnahme auf dem konkreten Snapdragon X.
- Erst WAV-Streaming und Deutsch, dann Mikrofon und weitere Sprachen.
  Keine GUI, Uebersetzung oder LLM-Anbindung im MVP. Eigene FastRPC-/HVX-Kernel
  sind nur fuer den explizit freigegebenen FP32-Pfad vorgesehen; eigene
  HMX-Kernel sind weiterhin kein MVP-Ziel.
- Keine stillen CPU-Fallbacks: Platzierung und Abweichungen sind sichtbar.
  Ein CPU-Referenzmodus ist ein expliziter Entwicklungs-/Diagnosemodus.

## Ausgangspunkt im Repository

Die bestehenden Whisper-, GLM-OCR- und TranslateGemma-Anwendungen zeigen,
wie freestanding ARM64 und direkter QNN-Zugriff hier zusammenspielen.
Vorhandene Infrastruktur gezielt wiederverwenden:

| Grundlage | Verwendung und Grenze |
| --- | --- |
| [QNN ABI](src/shared/qnn_abi.h), [Importdefinitionen](src/shared/imports), [QNN-Mock](src/shared/tests) | ABI, Laden und Fehlertests; Erweiterungen nur fuer tatsaechlich benoetigte Funktionen |
| [Whisper-Build](tools/whisper/build.ps1) | ARM64-Clang, freestanding-Flags, Stack-Probe und Linker als Vorbild, nicht den gesamten Build kopieren |
| [newos shared](../../src/shared) und [Windows-Backend](../../src/platform/windows) | Runtime, Speicher, Mathematik, I/O und Zeitmessung direkt einbinden |
| [Whisper-WAV](src/apps/whisper/whisper_wav.c) | RIFF-/WAV-Parsing als Vorbild; derzeit 16 kHz mono float32 und auf 30 s gepaddete Reads, kein PCM16-Streaming-Reader |
| [Whisper-Frontend](src/apps/whisper/whisper_frontend.c) | Numerische Hilfen pruefen; feste 400-Punkt-Transformation, globale Scratch-Puffer und Whisper-Normalisierung nicht als Streaming-Frontend uebernehmen |
| [Whisper-Encoder](src/apps/whisper/whisper_encoder_qnn.h) | Graph-Build/Restore und Pufferverwaltung als Vorbild; feste Schnittstelle ist kein Streaming-Cache-Vertrag |
| [Gemma-Runner](src/apps/translate/gemma_runner.c) | QPC-Zeitmessung und QNN-Shared-Buffer-Registrierung als Vorbilder; registrierte Puffer sind kein Nachweis device-residenter Zustaende |
| [QNN-Plattformnotizen](docs/platform/snapdragon-x-qnn.md) | Kontext-Kompatibilitaet, Spill/Fill, Restore-Kosten und teure kleine Decoder-Aufrufe beachten |
| [Zentraler Build](make.ps1) und [Runtime-Fetch](tools/shared/fetch-qnn-runtime.ps1) | Ab erstem nativen Probe-Build Clean-/Restore-Vertrag und Runtime-Staging pruefen; Default-Build und Produktionsdistribution erst nach passenden Gates erweitern |

Whispers Decoder ist kein RNN-T-Decoder. Seine KV-Cache-Logik darf nicht
unveraendert uebernommen werden. Gemeinsam sind die Notwendigkeit korrekter
Zustandsverwaltung und die Gefahr vieler kleiner NPU-Aufrufe.
Nicht den gesamten Whisper-Stack fuer Nemotron parametrisierbar umbauen.

### newos-Substrat und Plattformgrenze

[Runtime](../../src/shared/runtime.h), [Mathematik](../../src/shared/math/math.h)
und [Task-Pool](../../src/shared/concurrency.h) liefern Speicher, gepruefte
Array-Allokation, I/O, UTF-8, numerische Funktionen und CPU-Parallelisierung.
Diese Bausteine direkt einbinden, nicht privat nachbauen. Portable Frontend-,
Artifact- und RNN-T-Logik bleibt frei von Windows-/QNN-Typen; OS-Interaktion
geht ueber `platform.h`, notwendige QNN-/Windows-Spezialfunktionen ueber
schmale Adapter. Direkte Win32-Aufrufe benachbarter Tools sind kein Vorbild
fuer portable Modelllogik.

Die Windows-Implementierung von `platform_get_monotonic_time_ns` verwendet
derzeit `GetTickCount64`. Nanosekunden als Rueckgabeeinheit bedeuten hier
keine hochaufloesende Messung. Fuer Stage-, Dispatch- und Transfermessungen
einen QPC-basierten Adapter mit gepruefter Frequenz und expliziten Fehlerpfaden
wie bei den bestehenden Anwendungen verwenden.

`platform_open_read` verwendet derzeit `CreateFileA`. UTF-8-Transkripte
garantieren daher keine korrekte Behandlung von Unicode-Dateipfaden.
Den Windows-Pfadvertrag vor CLI/Deployment festlegen; notwendige UTF-8-/UTF-16-
Umsetzung in der Plattformgrenze halten und nicht-ASCII-WAV- sowie Modell-
und Deployment-Pfade testen.

## Modellvertrag vor Implementierung

Die Model Card beschreibt 600 Mio. Parameter, einen cache-aware
FastConformer-Encoder mit 24 Schichten, RNN-T und Sprach-Prompt-Conditioning.
Audio-Chunks von 80, 160, 320, 560 und 1120 ms werden angeboten.
Deutsch `de-DE` gehoert zu den direkt unterstuetzten Sprachen.
Chunkgroesse ist weder Rechenzeit noch garantierte Ende-zu-Ende-Latenz.

Die Model Card beschreibt Sprach-Conditioning auf dem akustischen Zweig:
Encoderrepraesentation und Sprachrepraesentation werden kombiniert und
projiziert. Dies ist nicht lediglich ein Prompt-Token zur Initialisierung
des Prediction-Netzes. Exakte Gewichte, Operationen und Platzierung werden
aus dem gepinnten Checkpoint bestaetigt, nicht aus der Grafik abgeleitet.

Das 320-ms-Startprofil entspricht laut Streaming-Anleitung
`att_context_size=[56,3]` in 80-ms-Encoderframes. Ein Profil ist der gesamte
Streaming-Vertrag, nicht allein eine Millisekundenangabe. Pro Profil separat
festhalten: neu konsumierte Audiosamples, erhaltenen Frontend-Overlap,
Encoder-Eingabeframes und gueltige neu ausgegebene Encoderframes. Frontend-
Overlap darf nicht zu doppelt konsumiertem Audio oder doppelt ausgegebenen
Frames fuehren.

Vor dem Schreiben der Inferenz wird aus dem gepinnten Checkpoint ermittelt:

- Exakte Sample-Rate, Fenster, Hop, FFT, Mel-Filter, Preemphasis, Dither,
  Log-Floor, Normalisierung, Padding und Streaming-Randbehandlung.
- Encoder-Subsampling, Lookahead, gueltige Ausgabeframes und alle
  Attention-/Convolution-Caches einschliesslich Laengen und Reset-Werten.
- Locale-zu-Sprach-ID-Abbildung, Conditioning-Tensoren, Gewichte und
  Projektion; Graphgrenze zwischen Encoder, Conditioning und Decoder.
  Sprachinput und ausgegebene Sprach-Tag-Tokens sind getrennte Vertraege.
- Prediction-Netz: Schichten, Gewichte, rekurrente Zustaende und Startzustand.
- Joiner, Blank-ID, Vokabular, Prompt-/Sprach-/Endtoken und Dekodieralgorithmus.
- Verhalten fuer erste, normale und letzte Chunks sowie Sprachende.
- Welche Normalisierung wirklich kausal ist. Keine Annahme, dass eine
  offline ueber die gesamte Aufnahme berechnete Normalisierung live funktioniert.

Erste, normale und letzte Chunks erhalten jeweils dokumentierte Formen,
gueltige Laengen, Padding- und Cache-Update-Regeln. Cache-Pufferbesitz,
Lebensdauer und erlaubtes beziehungsweise verbotenes Input-/Output-Aliasing
werden ebenfalls Teil des Vertrags.

Exportierte Graphen muessen diese Semantik erhalten. Ein erfolgreicher
Offline-Export ohne Cache-Ein-/Ausgaenge erfuellt das Streaming-Ziel nicht.

## Benoetigte Komponenten und NPU-Platzierung

```text
WAV / spaeter Mikrofon
  -> PCM-Ringpuffer und gegebenenfalls Resampling
  -> zustandsbehaftetes Audio-Frontend
  -> Streaming-Encoder + Attention-/Convolution-Caches [NPU]
  -> Sprach-Conditioning + akustische Projektion        [NPU angestrebt]
  -> RNN-T-Joiner                                      [NPU angestrebt]
       ^ Prediction-Netz + rekurrenter Zustand         [NPU angestrebt]
  -> Blank-/Token-Auswahl und RNN-T-Steuerung           [CPU]
  -> Token-Decodierung, partielle/finale UTF-8-Ausgabe   [CPU]
```

### Audio und Features

Zuerst ein eng definierter WAV-Eingang, vorzugsweise 16 kHz mono PCM16,
nach Bestaetigung der Modell-Sample-Rate. Nicht unterstuetzte Formate werden
explizit abgelehnt. Spaeter Capture mit begrenztem Ringpuffer,
Formatkonvertierung und einem getesteten Streaming-Resampler.
Windows-Audio-ABI/Importe werden erst nach Vergleich der vorhandenen Plattform
und der benoetigten Capture-Funktionen festgelegt.

Das Frontend braucht Fensterung, FFT/Leistungsspektrum, Mel-Projektion,
Logarithmus und die exakte Modellnormalisierung. Ueber Chunkgrenzen bleiben
Samples und notwendige Zustaende erhalten. Padding darf keine Doppelverarbeitung
oder kuenstliche Sprache erzeugen. Zuerst C-Referenz auf CPU; danach NPU-Offload
von Mel-Projektion/weiteren Operationen nur bei nachgewiesenem Gesamtnutzen.
Nemotron erhaelt ein eigenes zustandsbehaftetes Frontend; die feste
Whisper-Transformation und deren aufnahmeweite Normalisierung nicht
uebernehmen. Eine kleine allgemeine WAV-Reader-Extraktion ist frueh erlaubt,
wenn Whisper und Nemotron sie tatsaechlich gemeinsam nutzen und die bisherigen
Whisper-Formate und Tests erhalten bleiben.

### QNN und Modellartefakte

Die folgenden QNN-Vertraege beschreiben den vorhandenen Vergleichspfad.
Der freigegebene FastRPC-FP32-Pfad verwendet stattdessen validierte eigene
Graph-Artefakte und einen signierten DSP-Interpreter. Modelloperationen
laufen dort auf Hexagon/HVX; der Host steuert nur I/O, Zustandsuebergabe und
Diagnose. Kein heimlicher CPU-Inferenzpfad. Graph-, Modul-, Katalog- und
OEM-Treiber-Hashes sowie die tatsaechliche Platzierung werden separat
gebunden und berichtet. Speicherallokation und Mapping auf dem echten DSP
sind ein eigenes Gate, nicht aus erfolgreichen Host-Allokationen abzuleiten.

Bevorzugt vorab kompilierte Kontexte statt Graph-Finalisierung beim Start.
Ein separater Export-/Prepare-Pfad erstellt die Artefakte.
QNN-HTP fuehrt die Graphen direkt aus; ONNX ist hoechstens ein Export-Zwischenformat.
Nicht von einem NeMo-Export direkt auf QNN-Kompatibilitaet schliessen.

Ein versioniertes Manifest bindet Modellrevision, SHA-256, Tokenizer,
Frontendkonstanten, vollstaendige Streaming-Konfiguration einschliesslich
`att_context_size`, Subsampling, First-/Steady-/Final-Formen und gueltigen
Laengen, Sprach-Conditioning, Tensor-IDs/-Namen/-Formen/-Dtypes, Cache-Vertrag,
Graphnamen, Praezision und QNN-/QAIRT-/SoC-Kompatibilitaet.
Groessen und Offsets werden vor Allokation/Restore validiert.
Fehlerhafte oder inkompatible Pakete werden mit Diagnose abgelehnt.
Der minimale Kontext-Envelope- und Fixture-Reader entsteht bereits fuer M1;
der vollstaendige Runtime-Artifact-Reader wird in M2 darauf aufgebaut.
Graphkonstruktion/Prepare und produktives Restore bleiben getrennte Programme.

Feste Formen pro Chunkprofil sind der erste Kandidat; zunaechst nur 320 ms,
spaeter 160/80 ms. Gueltige Laengen und Tail-Padding sind explizite Eingaben.
Caches liegen in vorallokierten, moeglichst wiederverwendeten Puffern.
Device-residente Zustaende oder gemeinsame I/O-Puffer nur verwenden, wenn die
konkrete Windows-QNN-ABI dies erlaubt; Transfers und Kopien sonst mitmessen.
Zuerst die vorhandene Shared-Buffer-Registrierung im Gemma-Runner pruefen,
bevor die gemeinsame ABI erweitert wird. Shared-Buffer-Registrierung allein
beweist weder persistente Device-Zustaende noch vermiedene Transfers.

### RNN-T-Steuerung und Text

Das Prediction-Netz startet mit dem separat geprueften Referenzstartzustand;
Sprach-Conditioning wird am festgelegten akustischen Graphzweig ausgefuehrt.
Ein Blank schreitet im Audio fort und veraendert den festgeschriebenen
Prediction-Zustand nicht. Nur ein akzeptiertes Nicht-Blank aktualisiert
Tokenhistorie und Prediction-Zustand. Prediction darf nicht bei jedem Blank
unnoetig erneut berechnet werden. Kandidatenzustaende werden nicht vor der
Tokenentscheidung festgeschrieben. Ein Schutzlimit gegen Endlosschleifen
meldet Ueberschreitung als Fehler, nicht als erfolgreich gekuerztes Transkript.
Tie-Breaking bei gleichen Logits entspricht der Referenz; nichtendliche
Logits werden mit Diagnose abgelehnt.

Zuerst Greedy-Decoding entsprechend dem Referenzmodus, kein Beam Search.
CPU steuert Frames, Tokenwahl, Laengen und Ereignisse. Prediction und Joiner
werden separat und, soweit semantisch moeglich, kombiniert auf NPU geprueft.
Die rekurrente Abhaengigkeit verhindert beliebiges Batching zukuenftiger Tokens.

Tokenizer als vorbereitete, gepruefte Token-ID-zu-Byte-Tabelle, sofern der
Checkpoint dies ohne weitere Regeln erlaubt; andernfalls genau diese Regeln
in C implementieren. Sondertokens nicht als normalen Text ausgeben.
UTF-8, Subword-Grenzen, Sprach-Tags und Satzzeichen gegen Referenz pruefen.

Partielle Ausgabe und finale Ausgabe unterscheiden. Finalisierung erfolgt
bei explizitem EOF oder einer getesteten Endpointing-Regel; "Recognized"
oder ein ausgegebenes Token allein ist kein Nachweis eines Sprachendes.
Bei Reset werden alle Encoder-, Prediction-, Frontend- und Textzustaende
zurueckgesetzt. Silence/VAD darf im MVP keine Audioframes unbemerkt entfernen.

## Ablage: tool-intern oder newos shared

Empfohlener Start ohne Vorentscheidung fuer eine neue allgemeine ASR-Bibliothek:

```text
experimental/snapdragon/
  src/apps/nemotron/       CLI, Frontend, Modellvertrag, QNN, RNN-T, Tokenizer, Tests
  tools/nemotron/          Fetch, Referenz, Export, Prepare, Build, Benchmark
  docs/apps/              spaetere Implementierungs- und Messergebnisse
  models/nemotron-asr/    gepinnte Gewichte, Tokenizer, Referenzfixtures (ignoriert)
  build/nemotron-asr/     EXE, Objekte, Kontexte, Scratch (ignoriert)
  data/nemotron-asr/      Downloads und aufbewahrte Messungen (ignoriert)
```

Dies ist die vorgeschlagene, noch nicht angelegte Struktur.

Nach dem funktionierenden WAV-Pfad Entscheidung:

1. **Tool-intern:** Modellvertrag, RNN-T-Steuerung, Sprach-Conditioning und spezifische
   Cache-Layouts bleiben lokal. Das ist die Standardwahl bei nur einem Nutzer.
2. **`newos/src/shared`:** Nur nachgewiesen allgemeine Bausteine, etwa WAV,
   Streaming-FFT/Mel, Resampling oder Token-Byte-Ausgabe, mit eigenen Tests
   und ohne Windows-/QNN-Typen in der portablen Schnittstelle.
3. **Experimentelles shared:** Qualcomm-/Snapdragon-spezifische ABI und Loader
   gehoeren gegebenenfalls in `experimental/snapdragon/src/shared`, nicht
   automatisch in das plattformuebergreifende `newos/src/shared`.

Keine Kopien vorhandener Runtime-/Math-Helfer. Extraktionen muessen die
bisherigen Whisper-/OCR-/Translate-Builds und deren Verhalten erhalten.
Die kleine gemeinsam genutzte WAV-Extraktion ist die begrenzte Ausnahme zur
spaeteren Shared-Entscheidung, kein Anlass fuer eine allgemeine ASR-Bibliothek.
Neue oder entfernte gruppierte Shared-Quellen im kanonischen
[Source-Manifest](../../src/compiler/source_manifest.h) nachfuehren.

### Build, Clean und fruehes Deployment

Ab M1 gibt es einen eigenen freestanding Probe-Build unter `tools/nemotron/`
und einen isolierten Output unter `build/nemotron-asr/`. Reproduzierbare
Objekte, EXEs und Scratch bleiben dort; gepinnte Quellen, Referenzfixtures
und nicht reproduzierbare Eingaben liegen ausserhalb von `build/`.
Bei kompilierten Kontexten festlegen und testen, ob sie reproduzierbar neu
vorbereitet oder checksum-geprueft erhalten und restauriert werden.
Die derzeitige zentrale Restore-Auswahl erfasst verschachtelte Nemotron-
Kontexte nicht automatisch; ein blosses Ablegen unter `build/` reicht nicht.

Build-/Clean-/Restore-Checks beginnen mit dem ersten nativen Tool.
Nemotron bleibt zunaechst ein explizit aufgerufenes experimentelles Ziel;
den Default-All-App-Build und die Produktionsdistribution erst nach den
jeweiligen Korrektheits- und Deployment-Gates erweitern. Ab M3 den WAV-Pfad
aus einem isolierten Verzeichnis ohne Entwickler-PATH und Netzwerk pruefen,
einschliesslich Runtime-Staging, relativer Modellpfade, Unicode-Pfade,
DLL-Fehlern und Importaudit. M7 finalisiert die Distribution, statt diese
Vertraege erstmals festzulegen.

## Milestones und Abnahme

Die Reihenfolge ist ein kritischer Pfad. NPU-Machbarkeit wird vor grossem
Frontend-/CLI-Ausbau getestet; Qualitaet wird vor aggressiver Quantisierung fixiert.
Grenzwerte unten sind Projektziele, keine behaupteten Modellmessungen.

### M0 - Gepinnter Modellvertrag und reproduzierbare Referenz

Implementierung: [M0-Tooling und Referenzvertrag](docs/apps/nemotron-asr.md).
Fuer diesen Bring-up ist nach expliziter Entscheidung die gepinnte
Transformers-Streaming-Implementierung die ausfuehrbare Entwicklungsreferenz.
NeMo-Checkpoint, Konfiguration und Source-Revision werden ebenfalls gesichert;
numerische oder Token-Paritaet zwischen NeMo und Transformers wird nicht
vorausgesetzt. Der freestanding C-/QNN-Zielvertrag bleibt unveraendert.
M0-Abnahme wird erst durch den vollstaendigen Referenz-/Repeat- und
Inventarcheck dokumentiert, nicht allein durch vorhandene Skripte.
Der gepinnte Checkpoint listet Lookahead 3/0/6/13, nicht 1; die in der
Model Card beworbene 160-ms-Variante ist daher noch kein validiertes Profil.
Eine spaetere 160-ms-Abnahme braucht zuerst eine eigene Vertragspruefung.
M0-Referenzabnahme am 2026-10-13 bestanden: 16 Fixtures, 65 Chunks in der
langen Folge, exakte Tensor-/Token-/Text-Reproduktion in zwei Prozessen.
Der Bericht liegt unter `D:\newos-data\nemotron-asr\m0-acceptance.json`;
Messwerte und Grenzen stehen in der verlinkten Dokumentation.
Dies ist keine NeMo-Paritaets- oder HTP-Abnahme.

- Checkpointrevision, NeMo-Revision/Version, Entwicklungsumgebung, Lizenz
  und Hashes festhalten.
- Deutsch/320 ms mit explizitem `de-DE`, `att_context_size=[56,3]` und
  Greedy-Decoding als Startprofil am gepinnten Checkpoint bestaetigen.
- Kausale Frontend-Semantik, First-/Steady-/Final-Formen, Laengen,
  Cache-Updates, Pufferbesitz und Aliasing-Vertrag fixieren.
- Locale-/Sprach-ID-Vertrag, Conditioning-Gewichte, Projektion und
  Graphgrenzen festhalten; Sprachinput nicht mit Output-Tags verwechseln.
- Referenz fuer kurze Sprache, Pause, Stille, Chunkgrenzen und unvollstaendigen
  letzten Chunk sichern: Features, Caches, rohe und sprachkonditionierte
  Encoderrepraesentationen, Decoderzustaende, Token-IDs und finaler Text.
- Lange deterministische Chunkfolgen fuer akkumulierte Cache-/Feature-Drift
  sichern; Toleranzen und Folgenlaengen vor den nativen Vergleichen fixieren.
- Kleine deterministische Fixtures plus festes deutsches Qualitaetskorpus;
  Datenrechte und Referenztranskripte festhalten.
- QNN-/Treiber-/SoC-Version des Zielgeraets und DLL-Kette erfassen.

**Abnahme:** Referenz reproduzierbar; kein offener Streaming-/Conditioning-/
Cache-/Frontend-/Tokenizer-Vertrag. Die Revision ist ein Commit/Hash,
nicht ein bewegliches `main`.

### M1 - NPU-Machbarkeit des echten Streaming-Encoders

Implementierung und gemessene Grenzen:
[M1-Probe, Artefakte und Gate-Bericht](docs/apps/nemotron-m1.md).
Historischer QNN-Pfad: Der vollstaendige Encoder wurde auf HTP kompiliert
und mit fortgefuehrten Caches ueber 65 Chunks ausgefuehrt. Sein numerisches
Korrektheitsgate wurde nicht bestanden; das Zeitbudget allein war keine Abnahme.
Untersuchung am 2026-10-14: echte Benchmark-Laeufe ohne Vergleichs-/Profiling-
Overhead erreichen p95 Encoder plus Kopien 28,900/43,001/44,567 ms.
Per-Operator-Traces und feste/kanalweise Quantisierung wurden geprueft;
die unveraenderten FP32-M0-Toleranzen blieben auf diesem Pfad verletzt.
Dieser QNN-Pfad bleibt numerisch blockiert. Details und reproduzierbare
Diagnose-/Benchmark-Werkzeuge stehen im verlinkten Bericht.
Der anschliessend freigegebene Windows-only-FP32-Pfad ersetzt die
QNN-Prepare-/Restore-Anforderung fuer Nemotron durch einen eigenen
signierten FastRPC-Graph-Executor. Die bestehenden Korrektheits- und
Performance-Gates bleiben bestehen. Bei diesem Pfad muss der Bericht
Hexagon/HVX-Ausfuehrung, DSP-Speicherbedarf, Transport/Kopien und verbleibende
Host-Arbeit explizit ausweisen; keine QNN-/HMX-Platzierung behaupten.

Aktueller Status am 2026-10-14: M1 ist mit explizitem temporaerem
Latenz-Waiver des Nutzers vorlaeufig erreicht. Der signierte v17-FastRPC-
Snapshot besteht alle 65/3/4 Encoder-/Cache-Fixtures sowie Prediction und
Joiner mit null numerischen Verstoessen. Drei saubere warme p95-Laeufe
einschliesslich Subsampling und Cache-Kopien messen 323,756 / 328,518 /
326,650 ms. Das urspruengliche Ziel < 320 ms bleibt unerreicht; keine neue
330-ms-Grenze und keine numerische Lockerung. Der unveraenderte strikte
Gate-Bericht meldet weiter Performance pending. Freigabe und gepinnte
Artefakte stehen in `D:\newos-data\nemotron-asr\m1\provisional-acceptance-v17.json`.
Optimierung pausiert; v18-Sigmoid wurde wegen des zusaetzlichen strikten
Praezisionstests nicht uebernommen. Der Arbeitskernel entspricht wieder v17.

Abhaengigkeit: M0.

- Fuer den freigegebenen FastRPC-Pfad zuerst praezisionskritische Add-/MatMul-
  Fixtures auf dem echten signierten DSP-Modul pruefen, die bei FP16-Rundung
  die unveraenderten M0-Toleranzen verletzen. Build/Signatur allein genuegt nicht.
- Danach alle Modellgewichte, Attention-/Convolution-Caches und Workspace-
  Puffer real auf dem V73 abbilden. Grenzen einzelner RPC-Puffer und des
  32-Bit-DSP-Adressraums pruefen; fehlgeschlagene Mappings explizit melden.
  Graphen vollstaendig auf dem DSP ausfuehren, nicht pro Operator zu einer
  Host-Referenz ausweichen.
- Exportpfad mit allen Streaming-Zustaenden pruefen; bei fehlendem Export
  kleine Adapter oder direkte QNN-Graphkonstruktion untersuchen.
- Minimalen validierenden Kontext-Envelope-, Tensorvertrags- und Fixture-
  Reader vor dem ersten Restore bauen. Ungueltige Dimensionen, Offsets,
  Hashes, abgeschnittene Artefakte und Allokationsfehler explizit testen.
- Eigenen freestanding Probe-Build, Runtime-Staging und geprueften
  Build-/Clean-/Restore-Vertrag anlegen; keine Default-All-App-Erweiterung.
- Risikooperatoren und Cache-Updates zuerst als kleine Graphen testen,
  dann vollstaendigen Encoder fuer das 320-ms-Profil kompilieren.
- Separates freestanding ARM64-Probeprogramm: Kontext laden, First-/Steady-/
  Final-Chunks und lange Fixture-Folgen mit fortgefuehrten Caches ausfuehren.
  Conditioning-Projektion an der festgelegten Graphgrenze mitvalidieren.
  Cache-Pufferbesitz und erlaubtes/verbotenes Aliasing testen.
- Kleine Prediction-/Joiner-Probes mit echten Formen und Zustaenden:
  HTP-Operatorabdeckung, rekurrente Zustandsuebergabe und Dispatch-/Transfer-
  Overhead frueh pruefen. Volles Decoder-Offload und Optimierung bleiben M4;
  Decoderblocker getrennt vom Encoder-Gate berichten.
- NPU-Ausfuehrung, Operatorabdeckung, Spill/Fill, Speicher, Transfers und
  Warm-/Kaltlauf messen; keine versteckte ONNX-/CPU-Laufzeit.

**Korrektheitsgate:** Encoder, Conditioning an der vereinbarten Graphgrenze
und Caches stimmen innerhalb vorab anhand M0 definierter numerischer
Toleranzen ueberein; alle schweren Encoder-Operationen auf HTP.
Compile-Erfolg, ein einzelner Chunk oder nur wenige Chunks genuegen nicht.

**Performance-Status:** Warm p95 fuer Encoder einschliesslich Cache-Transfers
hat das Zwischenziel < 320 ms. Ergebnis und Decoder-Probe-Kosten separat
berichten; die vollstaendige Performance-Abnahme bleibt M6. Bei bestandenem
Korrektheitsgate, aber verfehltem Zwischenziel einen begrenzten Optimierungs-
versuch und eine explizite Weiter-/Stop-Entscheidung festhalten. Ein knapp
verfehltes Zeitbudget blockiert nicht automatisch die Frontend-Arbeit und
wird nicht als erreichtes Performance-Ziel ausgegeben.

**Bei Korrektheitsfehlern:** Praezision, Formen, Graphpartitionierung und
Operatorumschreibung begrenzt untersuchen. Blocker und CPU-Restanteil benennen;
ohne bestandenes Korrektheitsgate kein fertiges NPU-Ziel behaupten.

### M2 - Freestanding Frontend und vollstaendiger Artifact Reader

Status am 2026-10-14: implementiert und auf Windows ARM64 abgenommen.
[M2-Implementierung und Reproduktion](docs/apps/nemotron-m2.md) dokumentiert
16 eingefrorene M0-Fixtures mit drei Audio-Zulieferaufteilungen: 48 native
Laeufe, 5.216.256 Featurewerte, null Verstoesse gegen atol/rtol2e-5 und
byteidentische Ergebnisse zwischen den Aufteilungen. Der Frontend-Ring ist
auf512 Samples begrenzt; Reset, First/Steady/Final und EOF-Tails sind geprueft.
Alle16 Runtime-Ressourcen und Graph-/Cache-/Decoder-Vertraege, nicht-ASCII-
WAV-/Modellpfade, isoliertes Deployment, Fehlerpfade und byteidentisches
Clean/Rebuild/Restore bestehen. Eigener PE-Code: nur Kernel32, kein CRT.
Das Frontend rechnet derzeit auf CPU; keine neue Modellinferenz oder
HMX-/GPU-Platzierung. Die vorlaeufige M1-v17-Freigabe bleibt unveraendert.
Abnahmebericht: `D:\newos-data\nemotron-asr\m2\acceptance-final-2\report.json`.

Abhaengigkeit: M0 und bestandenes M1-Korrektheitsgate; dokumentierte
Weiterentscheidung bei verfehltem M1-Performance-Zwischenziel.

- Laufzeitgeruest mit newos-Speicher/I/O, WAV-Reader, Ringpuffer,
  Featureberechnung bauen; minimalen M1-Reader zum vollstaendigen validierenden
  Runtime-Artifact-Reader erweitern, nicht parallel neu implementieren.
- Feature-Paritaet fuer mehrere Audio-Zulieferaufteilungen innerhalb desselben
  Modellprofils, First/Steady/Final, Tail, Stille, Reset und lange Folgen.
  Keine Invarianz zwischen unterschiedlichen Lookahead-Profilen voraussetzen.
- Ungueltige WAVs, Dimensionen, Hashes, abgeschnittene Artefakte und
  Allokationsfehler mit expliziten Fehlerpfaden testen.
- Windows-Pfadvertrag umsetzen und nicht-ASCII-WAV-/Modellpfade testen.
  Build-/Clean-/Restore-Checks mit neuen Artefakten fortfuehren.

**Abnahme:** Features innerhalb fixierter absoluter/relativer Toleranzen;
keine Drift an Grenzen; eigener PE-Code ohne CRT/externes Inferenzframework.

### M3 - Vollstaendiger nativer WAV-Streaming-Pfad

Status am 2026-10-14: implementiert und abgenommen.
[M3-CLI, Ereignisse und Abnahme](docs/apps/nemotron-m3.md): alle16 M0-Fixtures
mit exakten Tokenfolgen und UTF8-Text; deutsche WER unveraendert8,376963%,
null absolute Prozentpunkte Regression. Encoder, akustische Projektion,
Prediction und Joiner laufen auf dem signierten v17-DSP, kein CPU-Modellfallback.
Blank-Reuse, Nicht-Blank-Commit, Tie-Breaking, nichtendliche Logits,
Schutzlimit, Cache-Aliasing, Reset plus zweite Aufnahme, relative und
nicht-ASCII-Pfade sowie byteidentisches Build/Clean/Restore bestehen.
Der OEM-Loader akzeptiert keine Unicode-Modulpfade; der Nutzer hat die
enge, sichtbare SHA-gepruefte temporaere ASCII-Staging-Grenze fuer unveraenderte
signierte Modul-/Katalogbytes freigegeben. Eigene Dateien werden danach
entfernt; keine Signing-/Treiber-/Boot-/Trust-Aenderung.
Bericht: `D:\newos-data\nemotron-asr\m3\acceptance-final-1\report.json`.
Mikrofon, Produktionsdistribution und finale Performance bleiben offen;
die vorlaeufige M1-Latenzfreigabe bleibt unveraendert.

Abhaengigkeit: M1 und M2.

- Freigegebenen FastRPC-FP32-Encoder mit Frontend verbinden; die bereits
  validierten Prediction-/Joiner-Graphen ebenfalls auf DSP verwenden.
- Greedy-RNN-T, akustisches Sprach-Conditioning, Prediction-Startzustand,
  Tokenizer, EOF-Flush, Reset und UTF-8-Ausgabe.
- Blank unveraenderter Prediction-Zustand, Nicht-Blank-Commit, deterministisches
  Tie-Breaking, nichtendliche Logits, Schutzlimit und Cache-Aliasing testen.
- Partielle/finale Ereignisse mit Audiozeit und monotoner Ausgabereihenfolge.
- CLI-Vertrag festlegen: WAV, Modellpaket, Chunkprofil, Sprache, Diagnose;
  Optionen sind geplant, nicht bereits vorhanden.
- Isolierten Offline-WAV-Deployment-Test ohne Entwickler-PATH und Netzwerk
  ausfuehren: relative Modellpfade, nicht-ASCII-Deployment-Pfad, passende
  QNN-DLLs, klare Fehlermeldungen und eigener PE-Code ohne CRT. Noch keine
  Produktionsdistribution oder Mikrofonabnahme behaupten.

**Abnahme:** Token-Paritaet auf deterministischen Fixtures; fuer das Korpus
maximal +1,0 Prozentpunkt absolute WER gegen dieselbe M0-Streaming-Referenz.
Normalisierung fuer WER explizit dokumentieren; UTF-8/Satzzeichen separat testen.
Reset plus zweite Aufnahme ist identisch zu einem frischen Prozess.
Build/Clean/Restore und isolierter WAV-Deployment-Test bestehen.

### M4 - Maximierung der NPU-Modellrechnung

Status am 2026-10-14: auf dem freigegebenen Windows-only-v17-FP32-Pfad
implementiert und abgenommen.
[M4-Optimierungen und Grenzen](docs/apps/nemotron-m4.md): alle16 M0-Fixtures
behalten exakte Tokenfolgen/UTF8-Text und WER8,376963%. Encoder, Conditioning/
Projektion, Prediction und Joiner bleiben DSP-Modellrechnung, ohne CPU-Fallback.
Exakte32-Spalten-Pakete der Decodergewichte, fusionierte Prediction/Joiner-
Aufrufe und persistente gemappte RPC-I/O vermeiden770 RPC-Aufrufe und
zusaetzliche Host-RPC-I/O-memcpy fuer10.390.023.336 gebundene Bytes.
Drei interleaved warme M3/M4-Prozesspaare messen nichtregressive Mediane:
Sprache15.464->14.121 ms, Stille1.400->1.306 ms, lang24.269->22.580 ms.
Drei64-Schritt-Numerikpruefungen, Reset, Unicode/Staging-Cleanup sowie
byteidentisches Clean/Rebuild/Restore bestehen. Signiertes v17-Modul,
Praezision, Cache-Flags und M1-Latenz-Waiver bleiben unveraendert.
Gemappte DDR ist kein behaupteter On-Chip-Zustand; Cache-Carry/Kontrolle/
Frontend/Text bleiben sichtbar Host-Arbeit. HMX, neue Quantisierung und
finale Echtzeitziele werden nicht als erreicht ausgegeben.
Bericht: `D:\newos-data\nemotron-asr\m4\acceptance-final-1\report.json`.

Abhaengigkeit: M3.

- Bereits validiertes DSP-Offload von Prediction/Joiner und Sprach-
  Conditioning/Projektion erhalten und gezielt optimieren.
  Praezision und Zustandsschleife separat validieren.
  Vergleich mit CPU-Referenz pro Frame/Token.
- Aufrufe fusionieren, Zwischenpuffer wiederverwenden, CPU-NPU-Kopien reduzieren.
- FP16/HTP-kompatible Ausgangspraezision vor INT8; keine pauschale Annahme,
  dass jede Operation FP16 unterstuetzt.
- Quantisierung mit repraesentativen, rechtlich nutzbaren Sprach-/Stille-/Cache-
  Eingaben, nicht nur zufaelligen Tensoren; Qualitaet erneut abnehmen.
- Frontend-Offload erst nach Encoder/Prediction/Joiner priorisieren.

**Abnahme:** Ziel ist Encoder samt Sprach-Conditioning/Projektion, Prediction
und Joiner auf NPU. Fuer jede
nicht offgeladene Modelloperation wird Grund, Rechenanteil und naechster
moeglicher Schritt dokumentiert. Gemessene End-to-End-Latenz darf gegen M3
nicht regressieren; ein schneller CPU-Modus rechtfertigt allein kein
Abhaken des maximalen NPU-Ziels.
Weiterhin <= +1,0 Prozentpunkt absolute WER gegen M0.

### M5 - Mikrofonbetrieb und Endpointing

Implementierung vorhanden;30-Minuten-Live-Abnahme noch offen.
[M5-Capture, Endpoint-Policy und Tests](docs/apps/nemotron-m5.md) beschreibt
WASAPI, begrenzte Queue, Capture/Inferenz im bestehenden Task-Pool und
statistikbasierte Tests ohne Audio-/Transkriptaufbewahrung. Der erste echte
Live-Versuch scheiterte nach807,820 Sekunden an Queue-Ueberlauf; Speicher
blieb stabil. Kein M5-Gate bestanden behaupten. Die Nutzerfreigabe erlaubt
vollstaendige interne Pausen, aber nur200ms Kontext nach bestaetigtem
Stille-Endpoint; sonstige Endpunktstille wird separat bilanziert.
EOF vor bestaetigtem Endpoint und der M0-WAV-Pfad bleiben unveraendert.
Nach einem weiteren Abbruch wegen Audio-Discontinuity absolvierte der
MMCSS-/Quellpositions-Retry1800,009 Sekunden Capture ohne Audiofehler,
Quellluecken oder Rueckstau:28800160 Samples vollstaendig bilanziert,
Queue-Peak14880/81920,21 abgeschlossene Aeusserungen.
Die unveraenderte Speichergrenze von2MiB wurde aber mit ca.7,03MiB
Wachstum zwischen den stationaeren Viertelmitteln ueberschritten.
`D:\newos-data\nemotron-asr\m5\acceptance-final-4\microphone-audit.json`
meldet deshalb weiterhin fehlgeschlagene Abnahme. Der Anstieg faellt mit
der ersten abgeschlossenen Aeusserung nach langer Leerlaufzeit zusammen;
First-use-Residency ist eine Hypothese, kein nachgewiesener Grund.
Speicherinitialisierung/Residency vor einem weiteren Abnahmeversuch untersuchen;
keine Lockerung von Warmup, Schwelle oder Audio-Kontinuitaetspruefung.

Abhaengigkeit: M4.

- Windows-Capture ohne Drittanbieter-DLL, Formatpruefung und Resampling.
- Capture-Thread getrennt von Inferenz; begrenzte Queues und Abbruch/Shutdown.
  CPU-Parallelisierung nutzt den vorhandenen Task-Pool; Capture-Lebenszyklus
  und Windows-Audio-Synchronisierung bleiben in einem schmalen Adapter,
  nicht in einer neuen privaten Threading-Runtime.
- Ueberlauf, Geraeteverlust und Audiofehler sichtbar melden; keine still
  verworfenen Samples. Audioaufnahme standardmaessig nicht auf Platte speichern.
- Endpointing mit konfigurierter Stillezeit; Pause innerhalb eines Satzes,
  lange Sprache, Hintergrundgeraeusche und Reset testen.

**Abnahme:** 30 Minuten Live-Betrieb ohne wachsenden Rueckstau, Zustand-
Vermischung oder Speicherwachstum; Fehler- und Abbruchtests bestehen.
Keine CPU-Windows-Spracherkennung als Ersatz fuer das Modell.

### M6 - Latenz-, Qualitaets- und NPU-Abnahme auf Snapdragon X

Abhaengigkeit: M5.

- 320-, 160- und 80-ms-Profile nur nach jeweiliger Paritaetspruefung vergleichen.
- Wiederholte/interleaved Messungen am Netz und auf Akku; Treiber, Runtime,
  Modellhash, Energieprofil, Warmup und Umgebung dokumentieren.
- Startzeit, Kontext-Restore, Frontend, Cache-Transfers, Encoder, Prediction,
  Sprach-Conditioning/Projektion, Joiner, Host-Decoding, Speicher und QNN-Aufrufe
  mit dem hochaufloesenden Messadapter getrennt erfassen.

**Projektziele fuer das ausgewaehlte Profil:**

- Gesamt-RTF <= 0,5: gesamte warme Inferenzzeit / verarbeitete Audiodauer.
- p95 Verarbeitungszeit je Chunk <= 80 % seiner Audiodauer und langfristig
  kein Rueckstau; nicht nur durchschnittliche NPU-Kernelzeit messen.
- p95 Verzoegerung fuer ein ausgegebenes Wort <= 500 ms ab dem Ende dieses
  Wortes in einer zeitlich annotierten Testaufnahme, mit Echtzeit-Zuspielung.
  Lookahead, Chunk-Pufferung und Queue-Wartezeit zaehlen mit.
- p95 Finalisierung <= 800 ms ab letztem Sprachsample; konfigurierte
  Endpoint-Stille zaehlt mit. EOF-Flush ist ein separater Messfall.
- <= +1,0 Prozentpunkt absolute deutsche WER gegen M0; absolute WER beider
  Systeme berichten. Referenzparitaet allein beweist keine brauchbare Qualitaet:
  eigenes Kommandokorpus vor Abnahme mit Ziel >= 95 % exakt erkannter Befehle.

**Abnahme:** Reproduzierbarer Bericht mit p50/p95, NPU-Platzierung,
Rest-CPU-Arbeit, RAM, Diskbedarf und erreichten/nicht erreichten Zielen.
H100-Benchmarks der Model Card sind kein Snapdragon-Nachweis.

### M7 - Shared-Entscheidung und minimale Distribution

Abhaengigkeit: M6.

- Tool-intern versus gezielte Shared-Extraktion anhand echter Wiederverwendung
  entscheiden; kein Umbau des gesamten ASR-Stacks ohne weiteren Nutzer.
- Bei Extraktion betroffene bestehende Tools gezielt bauen und testen.
- Bereits vorhandene Build-/Clean-/Restore- und WAV-Deployment-Vertraege
  finalisieren; zentrale Default-Build-/Produktions-Distro-Eintraege nach
  bestandenen Gates ergaenzen. Modelle und nicht reproduzierbare Eingaben
  bleiben vor Clean geschuetzt.
- EXE, passende QNN-DLLs, Modellpaket, Lizenz und Herkunftshinweise ausliefern.
  Keine Build-Skripte oder Entwicklungsumgebung fuer normalen Betrieb notwendig.
- Importtabelle und geladene DLLs pruefen; keine selbst hinzugefuegte CRT,
  ONNX Runtime oder Python. Fehlende DLLs mit klarer Diagnose behandeln.
- Aus einem frischen Deployment-Verzeichnis ohne Entwickler-PATH und ohne
  Netzwerk testen; vorhandener Qualcomm-Treiber bleibt Voraussetzung.

**Abnahme:** Offline-WAV und Mikrofon funktionieren aus der Distribution.
QNN-Kontext und Modell sind kompatibilitaetsgeprueft; Clean/Rebuild,
Fehlerszenarien, Lizenzmaterial und README sind konsistent.

## Risiken und Entscheidungsregeln

- **Exportunterstuetzung:** Nicht als gegeben behandeln. M1 ist der erste
  technische Go/No-Go, bevor eine grosse native Implementierung entsteht.
- **Cache-Traffic/VTCM:** 600 Mio. Parameter allein erlauben keine Aussage
  ueber Latenz oder Speicher. Aktivierungen und wiederholte Transfers messen.
- **Kleine Decodergraphen:** NPU-Auslastung ist nicht gleich Nutzen.
  M1-Probes fuer Prediction/Joiner frueh ausfuehren; CPU-Referenz beibehalten,
  NPU-Varianten ehrlich vergleichen. Registrierte Shared-Buffer nicht als
  Nachweis device-residenter Zustaende oder entfallener Transfers ausgeben.
- **Streaming-Semantik:** Offline-WER darf nicht als Streaming-WER dienen;
  dieselben Kontextprofile, Sprach-Conditioning-, Chunk-, End- und
  Decoderregeln vergleichen. Lange Folgen statt nur weniger Chunks pruefen.
- **Performance-Gates:** M1-Korrektheit, M1-Zwischenziel und M6-Endabnahme
  getrennt ausweisen. Weiterarbeit nach verfehltem Zwischenziel benoetigt
  eine benannte Entscheidung, keine stillschweigende Lockerung der Ziele.
- **80 ms:** Erst kleiner werden, wenn Paritaet und Dauerlast bestehen.
  Ein groesseres Profil ist ein benannter Kompromiss, kein versteckter Wechsel.
- **DLL-Ausnahme:** Transitive Qualcomm-Abhaengigkeiten separat ausweisen,
  nicht mit "dependency free" verschweigen.
- **Abnahme verfehlt:** Blocker und naechsten begrenzten Versuch dokumentieren;
  kein CPU-only-Ergebnis als fertigen NPU-Milestone deklarieren.

## Quellen und Lizenz

- [Nemotron 3.5 ASR Model Card](https://huggingface.co/nvidia/nemotron-3.5-asr-streaming-0.6b)
- [NeMo cache-aware Streaming-Referenz](https://github.com/NVIDIA-NeMo/NeMo/blob/main/examples/asr/asr_cache_aware_streaming/speech_to_text_cache_aware_streaming_infer.py)
- [OpenMDW-1.1](https://openmdw.ai/license/1-1/): kommerzielle Nutzung,
  Modifikation und Distribution erlaubt; Lizenz und zutreffende Herkunfts-/
  Copyright-Hinweise bei Weitergabe erhalten.
- Qualcomm-SDK/DLL-Lizenzen gelten separat. Runtime-Version und
  mitzuliefernde Lizenzdateien wie bei den vorhandenen Anwendungen erfassen.

Externe Quellen sind beweglich; M0 pinnt die tatsaechlich verwendeten
Revisionen und sichert den fuer das Modellpaket geltenden Lizenztext.
