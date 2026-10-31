# Iris

Assistente vocale stile J.A.R.V.I.S. che gira sul PC: la chiami con **"Iris"**,
capisce cosa le chiedi, usa internet quando serve e ti risponde a voce.
Stessa base di Voice to Chat: C++ nativo, un solo `.exe`, scheda video via Vulkan.

**Voce:** Qwen3-TTS 1.7B (Alibaba, licenza Apache 2.0) attraverso llama.cpp + mtmd, la
stessa libreria del modello di linguaggio. Voce di base del modello (femminile), circa 4 volte
più veloce del parlato, primo audio dopo circa 0,4 s. Usa 3 GB di memoria video (in tutto Iris
ne usa circa 9). Se manca o non riesce, parla la voce di Windows (Elsa/Cosimo). Si sceglie nella
pagina *Ascolto e voce*, insieme all'uscita audio.

**Sfera sopra i giochi**: la sfera della pagina Iris anche da sola, in una finestra senza sfondo
(trasparenza vera), sempre in primo piano, che si accende, gira e cambia colore con lo stato. I clic
la attraversano; si sblocca per spostarla, dimensione e opacità nelle *Impostazioni*. Non compare
nelle catture dello schermo (né in OBS, né nella lettura della chat).

**Effetto sulla voce** (stessa pagina), per una voce da macchina fatta apposta, con qualsiasi motore:
*IA di bordo* (risonanza metallica, voce sdoppiata, eco da plancia), *Androide* (ronzio elettrico
e tubo metallico), *Robot* (vocoder a nota fissa, monotono, da film). Elaborazione del suono nel
programma, niente modelli in più. Campioni in `campioni-voce\robotiche`.

```
microfono ─▶ Silero (c'è qualcuno che parla?) ─▶ frase intera ─▶ whisper large-v3-turbo
                                                                    │
                                     "Iris, ..." / finestra di ascolto / tasto per parlare
                                                                    ▼
                               Qwen2.5 7B (llama.cpp) ◀──▶ strumenti: ricerca web, pagine,
                                        │                  meteo, notizie, programmi, siti,
                                        ▼                  timer, volume, musica, memoria
                               voce neurale Qwen3-TTS (scheda video), frase per frase
                               (riserva: voce di Windows)
```

## Come si usa

- **"Iris, che tempo fa domani?"**: tutto di fila.
- **"Iris"** da sola, aspetti il *ding*, poi parli.
- **"Che ore sono, Iris?"**: va bene anche il nome alla fine.
- Dopo una risposta ti ascolta ancora per qualche secondo (6 di predefinito): rispondi senza ridire "Iris".
- **"Iris, basta"** (o "stop", "zitta") la interrompe; "Iris" mentre parla la ferma e ti ascolta.
- **"Iris, nuova conversazione"** ricomincia da capo.
- Facoltativo: un **tasto per parlare** (tastiera, mouse o joystick), nella pagina *Ascolto e voce*.
- Puoi anche **scriverle** nella casella in basso.
- **Quanto parla** (*Impostazioni*): frasi intere, brevi mentre giochi (predefinito) o brevi sempre. Brevi = una
  frase di poche parole, niente cortesie; i comandi di gioco confermano con una parola ("Rotta impostata.") o un
  suono. Il testo completo resta scritto nella conversazione.

Frasi da provare: "che tempo fa a Roma questo weekend?", "quali sono le notizie di oggi?",
"cerca chi ha vinto l'ultima gara di Formula 1", "apri Steam", "metti un timer di 10 minuti
per la pasta", "alza il volume", "metti in pausa la musica", "ricordati che il mio gioco
preferito è Star Citizen".

## Star Citizen

Il copilota della vecchia Iris (C#), rifatto dentro Iris e migliorato:

- **Comandi istantanei**: con il gioco aperto, le frasi del profilo ("Iris, carrello", "Iris, rotta
  per Crusader", "Iris, prepara la nave", "ancora") premono i tasti subito, senza modello
  (scancode, come una tastiera vera: Star Citizen ignora i tasti "virtuali").
- **Frasi libere**: quello che non è nel profilo lo capisce il modello, che ha lo strumento
  `game_command` ("alza gli scudi e accendi i motori" = due comandi).
- **Rotta sulla mappa stellare**: apre la mappa e aspetta la casella di ricerca, scrive il nome e
  verifica con l'OCR che sia entrato (se no riclicca e riscrive, l'ultima volta con un altro
  metodo), aspetta i risultati, clicca quello giusto, preme "Imposta rotta" e controlla che
  compaia "Annulla". Anche "annulla la rotta".
- **Nomi dei luoghi**: suggeriti a whisper quando il gioco è aperto, più alias e correzioni anche
  di più parole ("sera a film" -> Seraphim, "a nulla rotta" -> annulla rotta).
- **Amministratore**: se il gioco è avviato come amministratore e Iris no, Windows scarta i tasti
  senza errori: Iris lo controlla e lo dice.
- **Pagina Star Citizen**: stato, modalità prova (scrive cosa farebbe senza premere), prova di un
  comando scritto, calibrazione della mappa (Iris porta il gioco davanti, mouse sul punto + F8, un
  suono a ogni F8, poi torna lei davanti col risultato), "Leggi" per vedere cosa legge l'OCR in ogni
  zona (anche questo porta prima il gioco davanti: la cattura prende lo schermo così com'è), ultime
  azioni e frasi non capite.

Il profilo è `%appdata%\Iris\star_citizen.json` (tasti, frasi, alias, correzioni, calibrazione;
si può modificare a mano, anche con commenti, e Iris lo rilegge da sola). Al primo avvio importa
la vecchia Iris (`%appdata%\Iris\config`). Le immagini lette dall'OCR, con i riquadri, finiscono in
`%appdata%\Iris\debug`; le frasi non capite in `%appdata%\Iris\frasi_non_capite.txt`.
Il gioco va tenuto in finestra senza bordi (in schermo intero esclusivo la cattura esce nera).

## Chat di gioco (Voice to Chat dentro Iris)

Le funzioni di Voice to Chat ora sono dentro Iris, con **gli stessi modelli**: whisper e Qwen in
memoria una volta sola (prima, con i due programmi aperti, ce n'erano due copie: circa 6 GB di
scheda video in più). Voice to Chat resta com'era, ma **non va più aperto insieme a Iris** (i tasti
scatterebbero in tutti e due).

- **Detta in chat**: tieni premuto il tasto della chat, parli in italiano, il messaggio va nella chat
  del gioco già tradotto (gergo da videogiocatore: "sono a terra" -> "I'm down", "grazie, torno
  subito" -> "ty, brb"). F10 accende e spegne l'invio automatico anche in gioco.
- **"Iris, scrivi in chat: arrivo tra due minuti"**: stesso percorso, a voce o scritto.
- **Un tasto in comune** con Iris: con Star Citizen in primo piano la frase va in chat, a meno che
  tu dica "Iris"; fuori dal gioco va a Iris (così non scrive nel browser o in Discord).
- **Traduzione della chat degli altri** (OCR), nel riquadro sotto la chat; **messaggi filtrati** di
  un canale o di un'organizzazione; **glossario** (`%appdata%\Iris\glossary.txt`).
- **Una coda sola dei tasti verso il gioco**: un messaggio in chat e un comando della nave non si
  mescolano mai (niente "N" del carrello dentro una frase).
- Il modello traduce con due canali suoi (voce e chat letta), separati dalla conversazione di Iris:
  nessuno rifà il lavoro dell'altro. La lettura dello schermo si mette in pausa mentre parli o
  mentre Iris risponde.
- Al primo avvio Iris ha importato le impostazioni di Voice to Chat (lingue, tasti, chat, zona
  letta, glossario). Pagine *Chat di gioco*, *Traduzione chat*, *Messaggi filtrati*.

## Privacy

Riconoscimento della voce e ragionamento restano sul PC. In rete vanno solo, e solo quando
servono: le ricerche (DuckDuckGo, con Bing e Wikipedia di riserva), le pagine che Iris legge,
il meteo (Open-Meteo) e le notizie (Google News). Internet si spegne nelle *Impostazioni*.

## File

- `%appdata%\Iris\settings.cfg`: impostazioni.
- `%appdata%\Iris\memoria.txt`: le cose che le chiedi di ricordare (una per riga, modificabile).
- `%appdata%\Iris\log.txt`: registro (cosa ha sentito, che strumenti ha usato, tempi).
- `%appdata%\VoiceToChat\models`: i modelli, **condivisi con Voice to Chat** (stessi file),
  più i due della voce neurale (`Qwen3-TTS-12Hz-1.7B-Base-Q8_0.gguf`, `mmproj-...gguf`, 2,3 GB:
  si scaricano da soli al primo avvio se mancano).

## Compilare

Come Voice to Chat: Visual Studio (C++, "Sviluppo di applicazioni desktop con C++"), Vulkan SDK
(`winget install KhronosGroup.VulkanSDK`), poi `compila.bat`.
Risultato: `build\iris.exe` e `build\iris_tests.exe`.

llama.cpp e whisper.cpp sono sottomoduli git in `external\`, fissati alle versioni con cui Iris
compila (la voce neurale usa una parte recente di llama.cpp); miniaudio è un file solo, già dentro.
Per scaricare tutto insieme:

```
git clone --recursive https://github.com/MrRevotv/StarCopilot.git
```

(Già clonato senza `--recursive`: `git submodule update --init`.) I modelli (circa 6 GB, più 2,3 GB
per la voce neurale) si scaricano da soli al primo avvio in `%appdata%\VoiceToChat\models`.

## Prove

```
build\iris_tests.exe                         prove veloci (489 controlli, niente modelli né rete)
build\iris_tests.exe --completi              anche il modello vero e la rete
build\iris_tests.exe --ascolto               frasi dette dalla voce di Windows -> Silero -> whisper -> "Iris"
build\iris_tests.exe --chiedi "domanda" ...  domande a Iris dal terminale (modello + strumenti, senza voce)
build\iris_tests.exe --chiedi-breve "..."    lo stesso, con le risposte brevi (volume e programmi agiscono davvero!)
build\iris_tests.exe --traduci "frase" ...   la traduzione della chat col modello di Iris (voce, Iris in mezzo, chat letta)
build\iris_tests.exe --cerca "testo"         solo la ricerca su internet
build\iris_tests.exe --meteo "città"         solo il meteo
build\iris_tests.exe --notizie ["tema"]      solo le notizie
build\iris_tests.exe --app "nome"            che programma aprirebbe (senza aprirlo)
build\iris_tests.exe --voce "testo"          la voce di Windows
build\iris_tests.exe --voce-neurale t.txt o.wav  la voce neurale, frase per frase, in un file
build\iris_tests.exe --effetto robot in.wav out.wav  un effetto sulla voce (ia, androide, robot) su un WAV
build\iris_tests.exe --sfera sfera.png         un fotogramma della sfera fluttuante, su scacchiera (trasparenza)
build\iris_tests.exe --microfono "nome" [s]  volume e probabilità di voce del microfono, secondo per secondo
build\iris_tests.exe --trascrivi x.raw       cosa capisce whisper (audio float32 16 kHz)
```

## Codice (src)

| File | Cosa fa |
|---|---|
| `assistant.*` | il cuore: avvio, ascolto, "è per Iris?", richieste, timer, memoria |
| `agent.*` | la conversazione col modello: strumenti, frasi verso la voce, cronologia |
| `chat_format.*` | formato Qwen (ChatML, `<tool_call>`), pura |
| `tools.*` | gli strumenti (ricerca, meteo, notizie, programmi, timer...) |
| `llm_engine.*` | llama.cpp: risposta a pezzi, riuso della conversazione già elaborata |
| `voice_detector.*`, `vad_segmenter.*` | Silero in continuo e ritaglio delle frasi |
| `wake_word.*` | "Iris" all'inizio o alla fine della frase, comandi di stop |
| `tts_engine.*` | la voce: coda delle frasi, uscita audio, suoni; neurale o Windows (WinRT), interrompibile |
| `neural_voice.*` | Qwen3-TTS con llama.cpp + mtmd: testo -> audio a pezzi, sulla scheda video |
| `overlay.*` | la sfera fluttuante sopra i giochi (finestra layered, alfa per pixel, cliccabile solo da sbloccata) |
| `voice_effect.*` | effetti da macchina sulla voce (pettine, chorus, modulazione ad anello, vocoder, riverbero), puro |
| `speech_text.*` | frasi complete per la voce, testo "leggibile" (niente markdown, URL, emoji) |
| `html_utils.*`, `http_client.*` | pagine web, risultati di DuckDuckGo/Bing, RSS; WinHTTP |
| `system_actions.*` | programmi del menu Start (anche Store), volume, tasti multimediali |
| `game_profile.*` | il profilo di gioco (JSON tollerante), predefiniti di Star Citizen, importazione della vecchia Iris |
| `command_matcher.*` | dalla frase al comando: slot, alias, correzioni, Levenshtein + parole (puro) |
| `game_input.*` | scancode, combinazioni, digitazione, mouse, primo piano, controllo amministratore |
| `screen_ocr.*` | cattura + OCR di Windows con la posizione delle parole, immagini di controllo |
| `game_controller.*` | esecuzione dei comandi, rotta sulla mappa con verifiche e nuovi tentativi, calibrazione |
| `game_chat.*` | la chat di gioco (il motore di Voice to Chat): detta, traduzione, scrittura, lettura della chat degli altri |
| `ui/chat_pages.cpp` | le pagine Chat di gioco, Traduzione chat, Messaggi filtrati |
| copiati da Voice to Chat | `chat_parser`, `gamer_slang`, `glossary`, `input_sender`, `language_guess`, `languages`, `ocr_engine`, `ocr_overlay`, `region_selector`, `sc_locations`, `screen_capture`, `screen_match`, `translation_prompt` (con le loro prove) |
| `ui/` | finestra (stile Voice to Chat), sfera animata, conversazione a fumetti |
| da Voice to Chat | `audio_capture` (+ ascolto continuo), `whisper_engine`, `gpu`, `hotkey_listener`, `joystick_input`, `overlay`, `ui/ui`, `log`, `text_utils`... |

## Prossimi passi

- Modello più bravo con gli strumenti (es. Qwen3 14B: la scheda video ha spazio).
- Star Citizen: provare la rotta in gioco e ritoccare la calibrazione; OCR anche di HUD e MFD
  (carburante, scudi, distanza); profili per nave; macro con condizioni.
