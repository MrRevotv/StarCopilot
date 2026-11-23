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

**Ruota nel gioco**: col suo tasto (Shift+Tab di base) si apre al centro dello schermo e porta Iris
davanti al gioco, così il cursore si libera da solo, senza Alt-Tab. Punta una voce col mouse e clicca:
*Mining* (leggi adesso), *Registro*, *Raffineria*, *Missioni*, *Diario*, *Traduzione* (accesa/spenta),
*Assi* (overlay dell'Axis Visualizer), *Sfera*, *Iris* (la finestra). Di nuovo il tasto, Esc, il tasto
destro o un clic al centro ridanno il fuoco al gioco; di base il tasto vale solo con Star Citizen davanti
(negli altri programmi Shift+Tab resta libero). I pannelli si mettono in alto uno accanto all'altro, o
dove li hai trascinati l'ultima volta, e non tolgono il fuoco al gioco. Serve il gioco in finestra senza
bordi. *Nascondi tutto* (Num *, o dal menu dell'icona nell'area di notifica) toglie dallo schermo ruota,
pannelli, sfera e overlay (anche la chat tradotta e il riquadro del mining che arrivano dopo) finché non lo
ripremi. Esclusi dalle catture (OBS, OCR).

**Avvio con Windows** (Impostazioni › Avvio): Iris parte nascosta nell'area di notifica (chiave Run
dell'utente, niente permessi da amministratore). La X la nasconde lì (tasto destro sull'icona › Esci per
chiuderla); aprirla di nuovo mostra quella già attiva. Il saluto all'avvio è spento di base. Se appena
acceso il PC l'area di notifica non è ancora pronta, Iris riprova da sola ad aggiungere l'icona. Mentre è
nascosta (o ridotta a icona) la pagina si ferma e WebView2 tiene meno memoria. Quando Windows si spegne
Iris salva la finestra (anche ingrandita) e si chiude senza bloccarlo.

**Aggiornamento automatico**: all'avvio e ogni 4 ore Iris controlla l'ultima release su GitHub,
scarica l'exe nuovo, ne verifica l'impronta SHA-256 e lo installa da sola al riavvio successivo (o
subito con *Riavvia e aggiorna*). Si spegne in *Impostazioni*. Se GitHub limita i controlli (troppe
richieste, 403/429) Iris aspetta un'ora; se non risponde (5xx) o manca la rete riprova dopo mezz'ora, poi
sempre più piano (al massimo ogni 4 ore), e la pagina dice a che ora. L'exe nuovo si copia accanto a quello
in uso, si controlla e poi prende il suo posto con un solo cambio di nome; se una versione, installata, non
parte come quella versione, viene scartata e non si reinstalla in cerchio.

**Voce:** Qwen3-TTS 1.7B (Alibaba, licenza Apache 2.0) attraverso llama.cpp + mtmd, la
stessa libreria del modello di linguaggio. Circa 4 volte più veloce del parlato; il primo audio
arriva quando la frase è pronta (le frasi lunghe a blocchi di 5,8 s). Usa 3 GB di memoria video
(in tutto Iris ne usa circa 9). Si sceglie nella pagina *Ascolto e voce*, insieme all'uscita audio.

La voce è **sempre la stessa**: il modello "Base" imita una voce di riferimento (il codificatore di
parlante del mmproj ne ricava il timbro, un x-vector) e i semi sono fissi (stesso testo = stesso
audio). Senza riferimento si inventava un parlante a ogni frase, e a volte lo cambiava a metà.
Il riferimento è una delle **voci predefinite** (femminili, calme; la prima volta il modello la
genera da sé con un seme fisso, qualche secondo, e la salva in `%appdata%\Iris\voci`) oppure
**un tuo WAV** (5–15 s di parlato pulito di una persona sola). Mentre la voce neurale si carica le
frasi la aspettano; se una frase non le riesce si riprova con lei: la voce di Windows (Elsa/Cosimo)
parla solo se la neurale proprio non c'è. Prima della voce numeri, unità e alcune sigle diventano
parole ("16.960" → "sedicimila novecentosessanta", "2,5" → "due virgola cinque", "-12", "1.250 m/s",
"x4", "SCU", "QT", "ATC"...): la neurale leggeva "1.200" come "mul 200" e "SCU" come "scudi".
Misure e confronti: `iris_tests --voce-coerenza`, campioni in `campioni-voce\`.

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
- **"Iris, basta"** (o "stop", "zitta", "stai zitta!") la interrompe; "Iris" mentre parla la ferma e ti ascolta.
- **Frase a metà** ("Iris, imposta la rotta per..." e una pausa): suona il *ding* e aspetta il resto ("...Seraphim").
- Parlare *di* lei con gli amici non la sveglia ("ho aggiornato Iris", "parlavo con Iris"); nella finestra di
  ascolto "ehm", "mmm" e i pezzi di frase ("Integra...") non le arrivano. La sua voce tornata dalle casse si
  riconosce anche capita male.
- **"Iris, nuova conversazione"** ricomincia da capo.
- Facoltativo: un **tasto per parlare** (tastiera, mouse o joystick), nella pagina *Ascolto e voce*.
- Puoi anche **scriverle** nella casella in basso.
- **Quanto parla** (*Impostazioni*): frasi intere, brevi mentre giochi (predefinito) o brevi sempre. Brevi = una
  frase di poche parole, niente cortesie; i comandi di gioco confermano con una parola ("Rotta impostata.") o un
  suono. Il testo completo resta scritto nella conversazione.

Frasi da provare: "Iris, prepara la nave", "Iris, rotta per Seraphim", "Iris, che minerale è?",
"Iris, scrivi in chat: arrivo tra due minuti", "Iris, ricordati che il mio wingman si chiama Luca" (in memoria
subito, senza modello). Dopo un comando istantaneo il modello sa cosa è successo: "riprova" funziona.
Se le chiedi cose fuori dal gioco (meteo, aprire programmi...) ti dice che si occupa solo del gioco.

## Star Citizen

Il copilota della vecchia Iris (C#), rifatto dentro Iris e migliorato:

- **Comandi istantanei**: con il gioco aperto, le frasi del profilo ("Iris, carrello", "Iris, rotta
  per Crusader", "Iris, prepara la nave", "ancora") premono i tasti subito, senza modello
  (scancode, come una tastiera vera: Star Citizen ignora i tasti "virtuali").
- **Frasi libere**: quello che non è nel profilo lo capisce il modello, che ha lo strumento
  `game_command` ("alza gli scudi e accendi i motori" = due comandi).
- **Rotta sulla mappa stellare**: apre la mappa e aspetta la casella di ricerca (se non la vede non
  scrive niente: le lettere andrebbero alla nave), scrive il nome e verifica che sia entrato in due
  modi: l'OCR lo legge nel campo, oppure il campo è cambiato (il testo bianco sul vetro chiaro a
  volte l'OCR non lo legge, ma i tasti sono arrivati). Riscrive solo se il campo è rimasto com'era,
  così il nome non entra due volte; una ricerca di prima rimasta nel campo si cancella. Poi aspetta
  i risultati, clicca quello giusto, aspetta "Imposta rotta", lo preme e guarda per qualche secondo
  che compaia "Annulla". Ogni attesa è su quello che si vede, non a tempo; l'OCR prova più
  preparazioni dell'immagine (invertita, senza sfondo, solo i tratti chiari...) e conta uguali le
  lettere che confonde ("CRU-LI" = Cru-L1, "ANNULIA" = Annulla). Anche "annulla la rotta" (Esc solo
  se la mappa si è vista aperta: a mappa chiusa aprirebbe il menu del gioco).
- **Nomi dei luoghi**: suggeriti a whisper quando il gioco è aperto, più alias e correzioni anche
  di più parole ("sera a film" -> Seraphim, "a nulla rotta" -> annulla rotta). I numeri contano
  ("Cru L2" non diventa Cru-L1); ci sono tutti i punti di Lagrange ("cru l4", "hurston l2",
  "arc l uno") e i luoghi principali di Pyro.
- **Frasi storpiate e contrari**: whisper a volte sbaglia l'inizio ("Mi posso una rotta per
  Seraphim" era "imposta una rotta"): la parte fissa si cerca anche dopo qualche parola in più. Se
  ti correggi vale l'ultima frase ("Spegni le cose, come si chiamano? Spegni i motori."). I contrari
  non si confondono mai: "disattiva gli scudi" può diventare "abbassa gli scudi", mai "alza gli
  scudi"; "chiudi" mai "apri", "annulla la rotta per X" mai "rotta per X".
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
si può modificare a mano e Iris lo rilegge da sola). Accetta commenti (`//`, `/* */`, `#` a inizio
riga), virgole in più, una virgola dimenticata a fine riga e le virgolette “tipografiche”; se
qualcosa non va dice riga e colonna (e resta in uso il profilo di prima). Al primo avvio importa la
vecchia Iris (`%appdata%\Iris\config`). Quando Iris aggiunge comandi, frasi, tasti o luoghi nuovi,
un profilo di prima li riceve una volta sola, senza toccare i tuoi (il file di prima resta accanto,
`star_citizen.prima-dei-predefiniti-2.json`). Tasti predefiniti di Star Citizen 4.x: Alt+N chiama
la torre (anche "apri l'hangar"), Alt+C disaccoppiato, C cruise control, M modalità mining, F1
mobiGlas. Le immagini lette dall'OCR, con i riquadri, finiscono in `%appdata%\Iris\debug` (una per
zona, solo l'ultima lettura); le frasi non capite in `%appdata%\Iris\frasi_non_capite.txt`.
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
  mescolano mai (niente "N" del carrello dentro una frase). I tasti che Iris manda non fanno partire
  i tasti globali (il "-" di "CRU-L1" è il meno del tastierino, cioè il mining a mano).
- **Scrive solo nel gioco**: se al momento di scrivere Star Citizen non è davanti, se tieni premuto
  Shift, Ctrl o Alt, o se il gioco è da amministratore e Iris no, il messaggio tradotto resta nella
  cronologia e lo stato dice perché non è andato in chat.
- **Chat letta più pulita**: i canali letti male tornano col loro nome ("[GLCBALE]", "[BALE]",
  "[tGLOBA]" -> "[GLOBALE]"); due righe fuse dall'OCR si tagliano dove comincia la seconda; un
  messaggio già tradotto e riletto peggio dopo un po' non si ritraduce; "o7", "W", "?" non vanno al
  modello. L'ingrandimento per l'OCR si sceglie provando le due letture sulla stessa immagine (prima
  x2 e x3 si davano il cambio di continuo).
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
  Runtime (circa 21 MB, impronte SHA-256 fisse, ricontrollate prima di caricare la DLL) si scaricano
  da soli al primo avvio; se mancano (o manca il Visual C++ Redistributable) resta l'OCR di Windows.
  Si caricano nel thread del mining all'avvio, non nella pagina.
- **Annuncio a voce** (facoltativo): "Borase per 3", una volta per roccia, solo se la lettura è
  certa e senza alternative.
- **Rarità** del minerale (comune... leggendario) nelle informazioni complete, e i **depositi a
  terra** (ROC e a mano/FPS) nella tabella.
- **Letture sporche** ripulite: "2 16,960" (segnaposto letto come 2), "216,960", "l6,96O", lo zero
  col taglio dell'HUD letto "ø"; una cifra confusa dall'OCR (8/0/3/6...) si corregge solo se la
  correzione possibile è una sola, prima al posto delle lettere ("11,5SS" = 11.565), e il riquadro lo
  segna "probabile" (anche quando una lettera presa per cifra potrebbe dare un altro minerale). Le
  distanze dell'HUD ("19.5kM", "245m") e i numeri più piccoli della firma base più piccola (tacche,
  "100%") non sono signature. Valori con due possibilità (16.000 = Savrilium × 5 o Scrap × 8) le
  mostra tutte.
- **A mano**: Num - e poi le cifre dal tastierino, Invio conferma, Esc annulla. Mentre scrivi quei tasti
  NON arrivano al gioco (Invio aprirebbe la chat). A voce: "Iris, signature 16960" o "Iris, firma
  sedicimila novecentosessanta".
- **Lettura continua** (facoltativa): con Star Citizen davanti, ogni secondo; mostra un valore solo se
  letto due volte di fila, e se lo schermo non è cambiato riusa la lettura di prima senza rifare l'OCR.
  Un numero che non torna e non ha la forma del badge (un pezzo dell'HUD letto male) non si mostra.
- **Riquadro** accanto alla zona (spostabile, resta dove lo lasci; informazioni complete o base, cioè
  solo materiale e quantità; opacità dello sfondo e delle scritte separate, con lo sfondo a 0% solo le
  scritte con un alone scuro; durata) e **contorno della zona** disegnato FUORI
  dalla zona ed escluso dalle catture: non finisce mai nella lettura.
- **Tabella dei materiali** modificabile nella pagina *Mining* (`%appdata%\Iris\mining_materiali.csv`,
  al primo avvio quella dentro il programma): importa CSV, il `materialsdb` di Mining Signature
  Calculator o il `lookup_table.json` di `mining_ocr`; esporta CSV.
- **Sconosciute**: le signature che non tornano, lette pulite ("32,300") o scritte e dette, finiscono in
  `%appdata%\Iris\mining_sconosciute.csv` (restano dopo il riavvio); scrivi materiale e quantità e Iris
  aggiunge la firma base alla tabella. Un CSV salvato da Excel (codifica di Windows) si importa con gli
  accenti giusti.
- Immagini della lettura solo se le chiedi, e solo l'ultima (`%appdata%\Iris\debug\mining`).
- **Registro delle scansioni** (pagina *Mining* o voce *REGISTRO* della ruota): acceso,
  ogni roccia letta (tasto, lettura continua, voce, tastierino, barra) finisce nel registro, con
  esito (certo, probabile, ambiguo, sconosciuto), materiale, quantità, rarità; la stessa roccia
  riletta più volte conta una volta (anche tornandoci dopo un'altra), due rocce vicine di materiali
  diversi restano due. Alla chiusura Iris chiede **in che luogo eri** e la
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
disco fisso; poi controlla solo i rami della stessa installazione) o usa quello scelto, e segue il
file nuovo quando il gioco riparte. Un Game.log da 60.000 righe si rilegge in qualche decina di
millisecondi. Riconosce le
notifiche dell'HUD in italiano e in inglese (verificate su Game.log veri):

- **Missioni**: contratti accettati, completati, falliti (con il loro MissionId), obiettivi in corso;
  i contratti aperti restano dopo il riavvio (`missioni.json`, si dimenticano da soli dopo 3 giorni,
  anche con Iris sempre accesa).
- **Zona**: armistizio, spazio monitorato, proprietà privata, giurisdizione, ultima località, ultima nave pilotata.
- **Salute**: ferite (lieve/grave, parte del corpo, livello di trattamento), disidratato/fame, soccorsi in arrivo.
- **Gruppo**: un compagno in quantum verso di te. Accesso col tuo nome.
- **Avvisi a voce** scelti per categoria; la sessione già scritta si rilegge in silenzio.
- A voce: «Iris, che missioni ho?», «Iris, sono in zona di armistizio?» (strumento `game_status`).

Le uccisioni e gli ordini della raffineria **non** sono più nel Game.log delle versioni attuali.
La **raffineria** quindi va a timer (pagina *Mining*, ruota o a voce: «Iris, raffineria a
Levski tra 3 ore e 20»): conto alla rovescia, avviso a voce quando l'ordine è pronto anche a
finestra chiusa (e al risveglio, se è finito col PC sospeso; se è finito a Iris chiusa da più di 10
minuti, solo scritto), ordini salvati in `raffineria.json`. Durate: "3:20", "3h20", "200" (minuti),
"3 ore e 20", "un'ora e mezza", "un'ora e un quarto", "tre quarti d'ora", "45 minuti", "1g 2h".

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
  trasparente. *Solo OBS* manda i numeri a OBS senza mostrare niente sullo schermo. Risponde solo a
OBS e alle sue pagine: un sito aperto nel browser non può leggere assi e tasti (controllo di Host e
Origin, anche contro il "DNS rebinding").
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

- `%appdata%\Iris\settings.cfg`: impostazioni (salvate "tutto o niente": la versione di prima resta in
  `settings.cfg.bak` e si usa da sola se il file si rovina, per esempio per uno spegnimento a metà).
- `%appdata%\Iris\memoria.txt`: le cose che le chiedi di ricordare (una per riga, modificabile).
- `%appdata%\Iris\log.txt`: registro (cosa ha sentito, che strumenti ha usato, tempi).
- `%appdata%\Iris\aggiornamenti`: le versioni nuove scaricate e controllate, in attesa del riavvio
  (`scartate.txt`: quelle che, installate, non partivano come la loro versione).
- `%appdata%\Iris\WebView2`: i dati dell'interfaccia (cache di WebView2).
- `%appdata%\Iris\axis.json`: profili e moduli dell'Axis Visualizer (stesso formato di Axis Visualizer;
  copia di sicurezza `axis.json.bak`).
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
l'exe, come le pagine per OBS dell'Axis Visualizer (`src\ui\axis`). L'header C di ONNX Runtime
1.20.1 (licenza MIT) è già nel repository in `external\onnxruntime` (la DLL la scarica Iris).
Per lavorarci senza ricompilare: `build\iris.exe --interfaccia "C:\...\src\ui\web"` (le pagine
si leggono dal disco, F5 ricarica, F12 strumenti per sviluppatori). Se il processo della pagina di
WebView2 si chiude, Iris la ricarica; se si chiude tutto WebView2 (non per lo spegnimento di Windows) lo
ricrea con lo stesso stato; dopo tre volte in dieci minuti mostra un messaggio fermo (clic per riprovare)
e intanto voce, tasti e ruota continuano a funzionare. Il motivo finisce nel registro.

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
build\iris_tests.exe --prova-domande [breve]  un giro di frasi vere col gioco "aperto" (niente tasti): tempi, strumenti, risposte
build\iris_tests.exe --traduci "frase" ...   la traduzione della chat col modello di Iris (voce, Iris in mezzo, chat letta)
build\iris_tests.exe --voce "testo"          la voce di Windows
build\iris_tests.exe --voce-neurale t.txt o.wav  la voce neurale, frase per frase, in un file
build\iris_tests.exe --voce-coerenza [cartella] [voce] [t=0.9 k=50 ...]  la voce resta la stessa? 10 frasi:
                                             F0, variazione, centroide, timbro (x-vector), whisper; WAV salvati
build\iris_tests.exe --voce-semi 1 30 [cartella]  le voci che il modello si inventa seme per seme (per le predefinite)
build\iris_tests.exe --voce-confronta a.wav b.wav ...  quanto si somigliano i timbri di più WAV
build\iris_tests.exe --voce-leggi ["frase" ...]  cosa capisce whisper dalla voce neurale, testo grezzo e con numeri/sigle in parole
build\iris_tests.exe --voce-cuciture         clic tra un blocco di audio e l'altro della voce neurale?
build\iris_tests.exe --effetto robot in.wav out.wav  un effetto sulla voce (ia, androide, robot) su un WAV
build\iris_tests.exe --sfera sfera.png         un fotogramma della sfera fluttuante, su scacchiera (trasparenza)
build\iris_tests.exe --ruota cartella          la ruota e i pannelli in PNG (le finestre vere sono escluse dalle catture)
build\iris_tests.exe --microfono "nome" [s]  volume e probabilità di voce del microfono, secondo per secondo
build\iris_tests.exe --trascrivi x.raw       cosa capisce whisper (audio float32 16 kHz)
build\iris_tests.exe --comando "frase" [star_citizen.json]  che comando di gioco capisce Iris (senza premere niente)
build\iris_tests.exe --ocr-mappa img.png ["atteso"]  cosa legge l'OCR della mappa stellare con ogni preparazione dell'immagine
build\iris_tests.exe --mining "2 16,960"     cosa capisce la decodifica del mining da un testo dell'OCR
build\iris_tests.exe --mining-db materialsdb la tabella di Mining Signature Calculator, in chiaro
build\iris_tests.exe --mining-immagini dir   rilegge le catture salvate da MSC (logs\debug) e confronta
build\iris_tests.exe --ppocr                 PP-OCR contro l'OCR di Windows sui badge di prova (scarica il modello)
```

## Codice (src)

| File | Cosa fa |
|---|---|
| `assistant.*` | il cuore: avvio, ascolto, "è per Iris?", richieste, memoria |
| `agent.*` | la conversazione col modello: strumenti, frasi verso la voce, cronologia; si prepara mentre parli; non dice "fatto" se lo strumento non è stato usato |
| `chat_format.*` | formato Qwen (ChatML, `<tool_call>`), anche le richieste che il 7B scrive come testo (`game_command {...}`, `remember: fact=...`), pura |
| `tools.*` | gli strumenti del modello: memoria (+ game_command dal gioco) |
| `updater.*` | aggiornamento automatico: release di GitHub, download, SHA-256, sostituzione dell'exe |
| `llm_engine.*` | llama.cpp: risposta a pezzi, riuso della conversazione già elaborata, preparazione del prompt in anticipo |
| `voice_detector.*`, `vad_segmenter.*` | Silero in continuo e ritaglio delle frasi |
| `wake_word.*` | "Iris" all'inizio o alla fine della frase (non quando si parla di lei), comandi di stop, esitazioni e frasi a metà |
| `tts_engine.*` | la voce: coda delle frasi, uscita audio, suoni; neurale o Windows (WinRT), interrompibile |
| `neural_voice.*` | Qwen3-TTS con llama.cpp + mtmd: testo -> audio a pezzi, sulla scheda video; voce di riferimento (predefinite o tuo WAV), semi fissi; misure (F0, timbro) |
| `overlay.*` | la sfera fluttuante sopra i giochi (finestra layered, alfa per pixel, cliccabile solo da sbloccata) |
| `voice_effect.*` | effetti da macchina sulla voce (pettine, chorus, modulazione ad anello, vocoder, riverbero), puro |
| `speech_text.*` | frasi complete per la voce, testo "leggibile" (niente markdown, URL, emoji), numeri e sigle in parole |
| `http_client.*` | richieste web con WinHTTP (solo il controllo degli aggiornamenti) |
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
| `ui/quick_bar.*`, `ui/hud_host.*` | ruota (prende il primo piano e lo ridà al gioco) e pannelli sopra il gioco (GDI+, non rubano il fuoco) e cosa fanno |
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
