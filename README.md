# Iris

Assistente di gioco stile J.A.R.V.I.S. per **Star Citizen**: la chiami con **"Iris"**, comanda
la nave, detta e traduce la chat, legge le signature del mining e ti risponde a voce.
Stessa base di Voice to Chat: C++ nativo, un solo `.exe`, scheda video via Vulkan.

**Solo gioco**: Iris non controlla Windows (niente programmi, siti, volume, musica, timer) e non
usa internet per rispondere (niente ricerche, meteo, notizie). Le restano i comandi di gioco, la
chat, il mining e la memoria ("Iris, ricordati che...").

**Interfaccia** in stile HUD (pagina HTML/CSS/JS dentro WebView2, già presente in Windows 10/11),
servita dall'exe stesso: niente file in più da distribuire. Barra laterale con le pagine *Iris*,
*Star Citizen*, *Mining*, *Axis Visualizer*, *Diario di bordo*, *Chat di gioco*, *Traduzione chat*,
*Messaggi filtrati*, *Ascolto e voce*, *Impostazioni*; cursori, interruttori e la sfera animata che segue la voce.

**Barra rapida nel gioco** (l'idea della barra di citizen-assistant): col suo tasto (Num / di base)
scende dall'alto dello schermo, sopra il gioco, senza togliergli il fuoco: *Mining* (leggi adesso),
*Traduzione* (accesa/spenta), *Assi* (overlay dell'Axis Visualizer), *Sfera*, e i pannelli *Missioni*,
*Raffineria* e *Diario* (si trascinano dove vuoi e restano lì). *Nascondi tutto* (Num *) toglie dallo
schermo barra, pannelli, sfera e overlay finché non lo ripremi. Esclusa dalle catture (OBS, OCR).

**Avvio con Windows** (Impostazioni › Avvio): Iris parte nascosta nell'area di notifica (chiave Run
dell'utente, niente permessi da amministratore). La X la nasconde lì (tasto destro sull'icona › Esci per
chiuderla); aprirla di nuovo mostra quella già attiva. Il saluto all'avvio è spento di base.

**Aggiornamento automatico**: all'avvio e ogni 4 ore Iris controlla l'ultima release su GitHub,
scarica l'exe nuovo, ne verifica l'impronta SHA-256 e lo installa da sola al riavvio successivo (o
subito con *Riavvia e aggiorna*). Si spegne in *Impostazioni*.

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
                               Qwen2.5 7B (llama.cpp) ◀──▶ strumenti: comandi di gioco
                                        │                  (game_command), memoria
                                        ▼
                               voce neurale Qwen3-TTS (scheda video), frase per frase
                               (riserva: voce di Windows)
```

## Come si usa

- **"Iris, carrello"**: tutto di fila.
- **"Iris"** da sola, aspetti il *ding*, poi parli.
- **"Rotta per Crusader, Iris"**: va bene anche il nome alla fine.
- Dopo una risposta ti ascolta ancora per qualche secondo (6 di predefinito): rispondi senza ridire "Iris".
- **"Iris, basta"** (o "stop", "zitta") la interrompe; "Iris" mentre parla la ferma e ti ascolta.
- **"Iris, nuova conversazione"** ricomincia da capo.
- Facoltativo: un **tasto per parlare** (tastiera, mouse o joystick), nella pagina *Ascolto e voce*.
- Puoi anche **scriverle** nella casella in basso.
- **Quanto parla** (*Impostazioni*): frasi intere, brevi mentre giochi (predefinito) o brevi sempre. Brevi = una
  frase di poche parole, niente cortesie; i comandi di gioco confermano con una parola ("Rotta impostata.") o un
  suono. Il testo completo resta scritto nella conversazione.

Frasi da provare: "Iris, prepara la nave", "Iris, rotta per Seraphim", "Iris, che minerale è?",
"Iris, scrivi in chat: arrivo tra due minuti", "Iris, ricordati che il mio wingman si chiama Luca".
Se le chiedi cose fuori dal gioco (meteo, aprire programmi...) ti dice che si occupa solo del gioco.

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
- **Comandi nuovi senza toccare file**: nel pannello *Comandi* della pagina Star Citizen c'è
  l'elenco (con ricerca) e *+ Nuovo comando*: scegli cosa fa (premi un tasto, tieni premuto per
  qualche secondo, sequenza di tasti), scrivi le frasi, premi il tasto che usi in gioco (anche con
  Alt, Ctrl, Shift: lo scrive Iris) e, se vuoi, la risposta a voce. *Aggiunta rapida* propone idee
  pronte (porte, modalità mining/salvage/quantum, atterraggio automatico...) di cui scegli solo il
  tasto. Frasi doppie, tasti sbagliati o mancanti vengono segnalati prima di salvare; un tasto già
  nel profilo si riusa. Dei comandi speciali (rotta, mappa, salto quantico) si cambiano le frasi.

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
  un canale o di un'organizzazione; **glossario** (`%appdata%\Iris\glossary.txt`). Traduce solo i
  messaggi nuovi (quando la chat compare, solo l'ultimo): il riquadro ha la sua memoria, ogni
  traduzione nuova entra in fondo e spinge su le altre fino al numero scelto. La zona si può
  **girare** (rotellina del mouse nella scelta della zona) perché la chat dell'HUD segue la curva del
  casco: prima dell'OCR viene raddrizzata.
- **Una coda sola dei tasti verso il gioco**: un messaggio in chat e un comando della nave non si
  mescolano mai (niente "N" del carrello dentro una frase).
- Il modello traduce con due canali suoi (voce e chat letta), separati dalla conversazione di Iris:
  nessuno rifà il lavoro dell'altro. La lettura dello schermo si mette in pausa mentre parli o
  mentre Iris risponde.
- Al primo avvio Iris ha importato le impostazioni di Voice to Chat (lingue, tasti, chat, zona
  letta, glossario). Pagine *Chat di gioco*, *Traduzione chat*, *Messaggi filtrati*.

## Mining (Mining Signature Calculator dentro Iris)

Il vecchio Mining Signature Calculator (Python) rifatto dentro Iris, con l'algoritmo di `mining_ocr`.
Ti dice **a schermo** che minerale o oggetto c'è nella signature dello scanner, e quanti:
"Copper (Rame) × 4", "Scrap (salvage) × 3".

- **Lettura**: Num + (o un pulsante del joystick, o "Iris, che minerale è?"). Iris cattura la zona
  scelta, trova il badge (segnaposto + numero) con qualsiasi colore dell'HUD, toglie l'icona, lo
  pulisce (CLAHE, Otsu: il preprocessore di `mining_ocr` rifatto in C++ senza OpenCV) e lo legge
  prima con **PP-OCRv3** (il riconoscitore di PaddleOCR, lo stesso che usa citizen-assistant; ONNX
  Runtime caricato al volo, su processore) e, se la lettura non è pulita e sicura, con l'OCR di
  Windows su più versioni dell'immagine, tenendo quella che torna con la tabella. Modello e ONNX
  Runtime (circa 21 MB, impronte SHA-256 fisse) si scaricano da soli al primo avvio; se mancano (o
  manca il Visual C++ Redistributable) resta l'OCR di Windows.
- **Annuncio a voce** (facoltativo): "Borase per 3", una volta per roccia, solo se la lettura è
  certa e senza alternative.
- **Rarità** del minerale (comune... leggendario) nelle informazioni complete, e i **depositi a
  terra** (ROC e a mano/FPS) nella tabella.
- **Letture sporche** ripulite: "2 16,960" (segnaposto letto come 2), "216,960", "l6,96O"; una cifra
  confusa dall'OCR (8/0/3/6...) si corregge solo se la correzione possibile è una sola, e il riquadro
  lo segna "probabile". Valori con due possibilità (16.000 = Savrilium × 5 o Scrap × 8) le mostra tutte.
- **A mano**: Num - e poi le cifre dal tastierino, Invio conferma, Esc annulla. Mentre scrivi quei tasti
  NON arrivano al gioco (Invio aprirebbe la chat). A voce: "Iris, signature 16960" o "Iris, firma
  sedicimila novecentosessanta".
- **Lettura continua** (facoltativa): con Star Citizen davanti, ogni secondo; mostra un valore solo se
  letto due volte di fila, e salta l'OCR se lo schermo non è cambiato.
- **Riquadro** accanto alla zona (spostabile, resta dove lo lasci; informazioni complete o base, cioè
  solo materiale e quantità; opacità dello sfondo e delle scritte separate, con lo sfondo a 0% solo le
  scritte con un alone scuro; durata) e **contorno della zona** disegnato FUORI
  dalla zona ed escluso dalle catture: non finisce mai nella lettura.
- **Tabella dei materiali** modificabile nella pagina *Mining* (`%appdata%\Iris\mining_materiali.csv`,
  al primo avvio quella dentro il programma): importa CSV, il `materialsdb` di Mining Signature
  Calculator o il `lookup_table.json` di `mining_ocr`; esporta CSV.
- **Sconosciute**: le signature che non tornano finiscono in `%appdata%\Iris\mining_sconosciute.csv`
  (restano dopo il riavvio); scrivi materiale e quantità e Iris aggiunge la firma base alla tabella.
- Immagini della lettura solo se le chiedi, e solo l'ultima (`%appdata%\Iris\debug\mining`).
- **Registro delle scansioni** (pagina *Mining* o pulsante *REGISTRO* della barra rapida): acceso,
  ogni roccia letta (tasto, lettura continua, voce, tastierino, barra) finisce nel registro, con
  esito (certo, probabile, ambiguo, sconosciuto), materiale, quantità, rarità; la stessa roccia
  riletta più volte di seguito conta una volta. Alla chiusura Iris chiede **in che luogo eri** e la
  **qualità media** dei materiali (0-1000) e salva sul desktop `Iris mining AAAA-MM-GG HH-MM.json`
  (per il sito) e, se vuoi, lo stesso in `.csv` per Excel. Il registro in corso sopravvive a un
  riavvio. Il formato del JSON è spiegato in [docs/registro-mining.md](docs/registro-mining.md).

## Diario di bordo (Game.log) e raffineria

> **ATTENZIONE: la lettura del file Game.log è vietata dai Termini di Servizio di Star Citizen.**
> Queste funzioni sono spente in partenza e vanno attivate consapevolmente, a proprio rischio, dopo
> l'avviso nella pagina *Diario di bordo*. Lo sviluppatore non si assume alcuna responsabilità per
> eventuali conseguenze causate dall'uso improprio di queste funzioni.

Attivato, Iris legge il Game.log in sola lettura (niente memoria del gioco, niente input, niente
scritture nella cartella del gioco), lo trova da sola (LIVE, PTU, EPTU, HOTFIX, TECH-PREVIEW su ogni
disco fisso) o usa quello scelto, e segue il file nuovo quando il gioco riparte. Riconosce le
notifiche dell'HUD in italiano e in inglese (verificate su Game.log veri):

- **Missioni**: contratti accettati, completati, falliti (con il loro MissionId), obiettivi in corso;
  i contratti aperti restano dopo il riavvio (`missioni.json`, si dimenticano da soli dopo 3 giorni).
- **Zona**: armistizio, spazio monitorato, proprietà privata, giurisdizione, ultima località, ultima nave pilotata.
- **Salute**: ferite (lieve/grave, parte del corpo, livello di trattamento), disidratato/fame, soccorsi in arrivo.
- **Gruppo**: un compagno in quantum verso di te. Accesso col tuo nome.
- **Avvisi a voce** scelti per categoria; la sessione già scritta si rilegge in silenzio.
- A voce: «Iris, che missioni ho?», «Iris, sono in zona di armistizio?» (strumento `game_status`).

Le uccisioni e gli ordini della raffineria **non** sono più nel Game.log delle versioni attuali.
La **raffineria** quindi va a timer (pagina *Mining*, barra rapida o a voce: «Iris, raffineria a
Levski tra 3 ore e 20»): conto alla rovescia, avviso a voce quando l'ordine è pronto anche a
finestra chiusa, ordini salvati in `raffineria.json`.

## Axis Visualizer (dentro Iris)

Axis Visualizer rifatto dentro Iris: gli assi, le barre e i pulsanti di HOTAS, joystick, pad
(XInput), mouse e tastiera, in finestre sopra il gioco e nelle sorgenti Browser di OBS.

- **Moduli**: *Assi* (puntino con la scia, terzo asse come barra, percentuali), *Barre* (manetta,
  timone, cursori), *Pulsanti* (caselle che si accendono e sfumano, cappelli, rotella del mouse;
  "Aggiungi premendo"). Nomi a schermo, colori, larghezza, allineamento del titolo, fino a 10 per
  profilo; lo stesso numero di *Finestra* li mette insieme. Anteprima dal vivo nella pagina.
- **Overlay**: clic che passano al gioco, trascinamento col tasto **centrale** (posizioni salvate),
  doppio clic centrale per chiudere, *Blocca la posizione*, *Nascondi*. Si apre da solo quando parte
  uno dei giochi del profilo (es. `StarCitizen.exe`) e si chiude con lui.
- **Profili**: uno per gioco, con i suoi moduli e i suoi giochi.
- **OBS**: server solo locale (127.0.0.1 e ::1) sulla porta **8787**, un indirizzo per finestra
  (`http://localhost:8787/g1`), stessa pagina e stesso protocollo di Axis Visualizer, sfondo
  trasparente. *Solo OBS* manda i numeri a OBS senza mostrare niente sullo schermo.
- **Importazione**: al primo avvio (o con *Importa da Axis Visualizer*) Iris copia profili, moduli,
  nomi, colori e posizioni da `%appdata%\AxisVisualizer\settings.json`. La porta resta diversa
  (Axis Visualizer usa la 8777), così i due programmi non si pestano i piedi.

## Privacy

Riconoscimento della voce, ragionamento e traduzioni restano sul PC. In rete vanno solo il
controllo degli aggiornamenti (l'API pubblica di GitHub, senza dati tuoi) e, al primo avvio, il
download dei modelli (anche PP-OCR e ONNX Runtime per il mining). Il server per OBS dell'Axis
Visualizer ascolta solo su questo computer.

## Pubblicare una versione

Gli utenti ricevono da soli ogni release pubblicata su GitHub (`MrRevotv/StarCopilot`) con l'exe
allegato. Per pubblicarne una:

```
pubblica.bat 1.3.0 "Cosa cambia in questa versione"
```

Scrive la versione nel file `VERSION` (entra nell'exe), compila, fa girare le prove e crea la
release `V1.3.0` con `build\iris.exe` (serve GitHub CLI: `winget install GitHub.cli`, poi
`gh auth login`). La versione deve essere più alta dell'ultima pubblicata. Poi commit e push del
codice. Chi ha una versione senza aggiornamento automatico (fino alla 1.1) deve scaricare a mano
la prima volta.

## File

- `%appdata%\Iris\settings.cfg`: impostazioni.
- `%appdata%\Iris\memoria.txt`: le cose che le chiedi di ricordare (una per riga, modificabile).
- `%appdata%\Iris\log.txt`: registro (cosa ha sentito, che strumenti ha usato, tempi).
- `%appdata%\Iris\aggiornamenti`: le versioni nuove scaricate e controllate, in attesa del riavvio.
- `%appdata%\Iris\WebView2`: i dati dell'interfaccia (cache di WebView2).
- `%appdata%\Iris\axis.json`: profili e moduli dell'Axis Visualizer (stesso formato di Axis Visualizer).
- `%appdata%\Iris\missioni.json`: i contratti aperti visti nel Game.log (solo col diario di bordo attivo).
- `%appdata%\Iris\raffineria.json`: gli ordini della raffineria con il loro conto alla rovescia.
- `%appdata%\Iris\registro_mining.json`: il registro delle scansioni in corso (il file finale va sul desktop).
- `%appdata%\VoiceToChat\models\ppocr`: PP-OCRv3 e ONNX Runtime per l'OCR del mining.
- `%appdata%\VoiceToChat\models`: i modelli, **condivisi con Voice to Chat** (stessi file),
  più i due della voce neurale (`Qwen3-TTS-12Hz-1.7B-Base-Q8_0.gguf`, `mmproj-...gguf`, 2,3 GB:
  si scaricano da soli al primo avvio se mancano).

## Compilare

Come Voice to Chat: Visual Studio (C++, "Sviluppo di applicazioni desktop con C++"), Vulkan SDK
(`winget install KhronosGroup.VulkanSDK`), poi `compila.bat` (la prima volta scarica da NuGet il
kit di WebView2 in `external\webview2`: solo header e libreria statica).
Risultato: `build\iris.exe` e `build\iris_tests.exe`.

L'interfaccia sta in `src\ui\web` (`index.html`, `app.css`, `app.js`, `axis.js`, `log.js`) e finisce dentro
l'exe, come le pagine per OBS dell'Axis Visualizer (`src\ui\axis`). La prima volta `compila.bat`
scarica anche l'header C di ONNX Runtime in `external\onnxruntime` (la DLL la scarica Iris).
Per lavorarci senza ricompilare: `build\iris.exe --interfaccia "C:\...\src\ui\web"` (le pagine
si leggono dal disco, F5 ricarica, F12 strumenti per sviluppatori).

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
build\iris_tests.exe                         prove veloci (niente modelli né rete)
build\iris_tests.exe --completi              anche il modello vero
build\iris_tests.exe --aggiornamento [scarica]  cosa vede l'aggiornamento su GitHub (e prova il download controllato)
build\iris_tests.exe --ascolto               frasi dette dalla voce di Windows -> Silero -> whisper -> "Iris"
build\iris_tests.exe --chiedi "domanda" ...  domande a Iris dal terminale (modello + strumenti, senza voce)
build\iris_tests.exe --chiedi-breve "..."    lo stesso, con le risposte brevi
build\iris_tests.exe --traduci "frase" ...   la traduzione della chat col modello di Iris (voce, Iris in mezzo, chat letta)
build\iris_tests.exe --voce "testo"          la voce di Windows
build\iris_tests.exe --voce-neurale t.txt o.wav  la voce neurale, frase per frase, in un file
build\iris_tests.exe --effetto robot in.wav out.wav  un effetto sulla voce (ia, androide, robot) su un WAV
build\iris_tests.exe --sfera sfera.png         un fotogramma della sfera fluttuante, su scacchiera (trasparenza)
build\iris_tests.exe --barra cartella          la barra rapida e i pannelli in PNG (le finestre vere sono escluse dalle catture)
build\iris_tests.exe --microfono "nome" [s]  volume e probabilità di voce del microfono, secondo per secondo
build\iris_tests.exe --trascrivi x.raw       cosa capisce whisper (audio float32 16 kHz)
build\iris_tests.exe --mining "2 16,960"     cosa capisce la decodifica del mining da un testo dell'OCR
build\iris_tests.exe --mining-db materialsdb la tabella di Mining Signature Calculator, in chiaro
build\iris_tests.exe --mining-immagini dir   rilegge le catture salvate da MSC (logs\debug) e confronta
build\iris_tests.exe --ppocr                 PP-OCR contro l'OCR di Windows sui badge di prova (scarica il modello)
```

## Codice (src)

| File | Cosa fa |
|---|---|
| `assistant.*` | il cuore: avvio, ascolto, "è per Iris?", richieste, memoria |
| `agent.*` | la conversazione col modello: strumenti, frasi verso la voce, cronologia |
| `chat_format.*` | formato Qwen (ChatML, `<tool_call>`), pura |
| `tools.*` | gli strumenti del modello: memoria (+ game_command dal gioco) |
| `updater.*` | aggiornamento automatico: release di GitHub, download, SHA-256, sostituzione dell'exe |
| `llm_engine.*` | llama.cpp: risposta a pezzi, riuso della conversazione già elaborata |
| `voice_detector.*`, `vad_segmenter.*` | Silero in continuo e ritaglio delle frasi |
| `wake_word.*` | "Iris" all'inizio o alla fine della frase, comandi di stop |
| `tts_engine.*` | la voce: coda delle frasi, uscita audio, suoni; neurale o Windows (WinRT), interrompibile |
| `neural_voice.*` | Qwen3-TTS con llama.cpp + mtmd: testo -> audio a pezzi, sulla scheda video |
| `overlay.*` | la sfera fluttuante sopra i giochi (finestra layered, alfa per pixel, cliccabile solo da sbloccata) |
| `voice_effect.*` | effetti da macchina sulla voce (pettine, chorus, modulazione ad anello, vocoder, riverbero), puro |
| `speech_text.*` | frasi complete per la voce, testo "leggibile" (niente markdown, URL, emoji) |
| `html_utils.*`, `http_client.*` | richieste web con WinHTTP (aggiornamenti), testo delle pagine |
| `game_profile.*` | il profilo di gioco (JSON tollerante), predefiniti di Star Citizen, importazione della vecchia Iris |
| `command_matcher.*` | dalla frase al comando: slot, alias, correzioni, Levenshtein + parole (puro) |
| `game_input.*` | scancode, combinazioni, digitazione, mouse, primo piano, controllo amministratore |
| `screen_ocr.*` | cattura + OCR di Windows con la posizione delle parole, immagini di controllo |
| `game_controller.*` | esecuzione dei comandi, rotta sulla mappa con verifiche e nuovi tentativi, calibrazione |
| `game_chat.*` | la chat di gioco (il motore di Voice to Chat): detta, traduzione, scrittura, lettura della chat degli altri |
| `mining_decoder.*` | mining: tabella dei materiali, numeri dal testo dell'OCR (pulizia e correzioni), voce, numeri in lettere (puro) |
| `mining_image.*` | mining: il preprocessore di `mining_ocr` in C++ (badge, icona, CLAHE, Otsu), BMP/PNG |
| `mining_scanner.*` | mining: letture (OCR di Windows su più versioni), lettura continua, tastierino che non arriva al gioco, file |
| `mining_overlay.*` | mining: riquadro a schermo, contorno della zona (escluso dalle catture) |
| `ppocr.*` | mining: PP-OCRv3 con ONNX Runtime caricato al volo (download controllato, decodifica CTC) |
| `game_log.*` | diario di bordo: lettura del Game.log (riconoscimento delle righe, stato, file che segue il gioco); spento finché non lo attivi |
| `refinery.*` | raffineria: timer degli ordini, durate scritte o dette («3 ore e 20»), salvataggio |
| `mining_log.*` | registro delle scansioni del mining: rilevamenti, riepilogo, file JSON/CSV sul desktop |
| `autostart.*` | avvio con Windows (chiave Run dell'utente) |
| `axis_config.*` | Axis Visualizer: profili e moduli (JSON di Axis Visualizer), importazione, finestre per OBS |
| `axis_input.*` | Axis Visualizer: periferiche (DirectInput, XInput, mouse, tastiera) e i loro valori |
| `axis_server.*` | Axis Visualizer: server per OBS (HTTP + WebSocket, solo locale) |
| `ui/quick_bar.*`, `ui/hud_host.*` | barra rapida e pannelli sopra il gioco (GDI+, non rubano il fuoco) e cosa fanno |
| `ui/axis_panes.*`, `ui/axis_overlay.*` | Axis Visualizer: disegno dei moduli (GDI+) e finestre sopra il gioco, in un thread suo |
| copiati da Voice to Chat | `chat_parser`, `gamer_slang`, `glossary`, `input_sender`, `language_guess`, `languages`, `ocr_engine`, `ocr_overlay`, `region_selector`, `sc_locations`, `screen_capture`, `screen_match`, `translation_prompt` (con le loro prove) |
| `ui/main_window.*` | la finestra con WebView2: serve le pagine dall'exe, tiene aggiornati i riquadri sopra il gioco |
| `ui/web_bridge.*` | il ponte pagina <-> motore: stato in JSON verso la pagina, comandi della pagina verso Iris |
| `ui/web/` | l'interfaccia HUD: `index.html`, `app.css`, `app.js` (pagine, cursori, sfera animata), `axis.js` (pagina e anteprime dell'Axis Visualizer) |
| `ui/axis/` | le pagine per OBS dell'Axis Visualizer (`overlay.html`, `index.html`) |
| da Voice to Chat | `audio_capture` (+ ascolto continuo), `whisper_engine`, `gpu`, `hotkey_listener`, `joystick_input`, `overlay`, `ui/ui`, `log`, `text_utils`... |

## Sviluppo con Claude Code

In `.claude/skills` ci sono le regole del progetto che Claude Code carica da solo: `ui-ux-design`
(stile HUD, stati dei controlli, verifica a vista), `windows-system-integration` (avvio, area di
notifica, aggiornamenti, tasti) e `ocr-architecture` (OCR di Windows, PP-OCR, pre-elaborazione).

## Prossimi passi

- Modello più bravo con gli strumenti (es. Qwen3 14B: la scheda video ha spazio).
- Star Citizen: provare la rotta in gioco e ritoccare la calibrazione; OCR anche di HUD e MFD
  (carburante, scudi, distanza); profili per nave; macro con condizioni.
