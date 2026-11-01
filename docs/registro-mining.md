# Registro delle scansioni del mining (file JSON di Iris)

Con **Registra le scansioni** acceso (pagina *Mining* o pulsante **REGISTRO** della barra rapida)
Iris annota ogni roccia letta dallo scanner. Alla chiusura chiede **luogo** e **qualità media** e
salva sul desktop:

- `Iris mining AAAA-MM-GG HH-MM.json`: il file per il sito (questo documento);
- `Iris mining AAAA-MM-GG HH-MM.csv` (facoltativo): lo stesso registro per Excel (`;`, UTF-8).

Se il nome esiste già si aggiunge ` (2)`, ` (3)`… Il JSON è UTF-8 senza BOM.

## Esempio

```json
{
  "formato": "iris-registro-mining",
  "versione": 1,
  "programma": "Iris 1.2.0",
  "operatore": "MrRevo",
  "luogo": "Aaron Halo",
  "qualitaMedia": 640,
  "inizio": "2026-11-01T21:40:12+01:00",
  "fine": "2026-11-01T22:55:03+01:00",
  "durataMinuti": 75,
  "riepilogo": {
    "rilevamenti": 5,
    "letture": 7,
    "certi": 3,
    "daVerificare": 2,
    "materiali": [
      { "materiale": "Copper", "nome": "Copper (Rame)", "tipo": "minerale", "rarita": "comune", "rocce": 2, "quantita": 8 },
      { "materiale": "Quantanium", "nome": "Quantanium", "tipo": "minerale", "rarita": "leggendario", "rocce": 1, "quantita": 3 }
    ]
  },
  "rilevamenti": [
    { "ora": "2026-11-01T21:40:17+01:00", "segnale": 16960, "esito": "certo", "materiale": "Copper", "nome": "Copper (Rame)",
      "quantita": 4, "tipo": "minerale", "rarita": "comune", "origine": "continua", "letture": 3 },
    { "ora": "2026-11-01T21:44:32+01:00", "segnale": 16000, "esito": "ambiguo",
      "alternative": [
        { "materiale": "Savrilium", "nome": "Savrilium", "quantita": 5, "tipo": "minerale" },
        { "materiale": "Scrap", "nome": "Scrap", "quantita": 8, "tipo": "salvage" }
      ],
      "origine": "tasto", "letture": 1 },
    { "ora": "2026-11-01T21:45:42+01:00", "segnale": 13337, "esito": "sconosciuto", "origine": "voce", "letture": 1 }
  ]
}
```

## Campi

| Campo | Tipo | Significato |
|---|---|---|
| `formato` | testo | sempre `"iris-registro-mining"`: serve al sito per riconoscere il file |
| `versione` | numero | versione del formato (oggi `1`); campi nuovi si aggiungeranno senza togliere questi |
| `programma` | testo | versione di Iris che l'ha scritto |
| `operatore` | testo | chi ha minato: il nome nel gioco (dal diario di bordo, se acceso) o il nome impostato in Iris; può essere vuoto |
| `luogo` | testo | scritto dall'operatore alla chiusura (es. "Aaron Halo", "Daymar") |
| `qualitaMedia` | numero 0–1000 | la qualità media dei materiali, scritta dall'operatore alla chiusura |
| `inizio`, `fine` | data ISO 8601 | ora locale con il fuso (`+01:00`, `+02:00` d'estate) |
| `durataMinuti` | numero | fine − inizio, arrotondata al minuto |
| `riepilogo.rilevamenti` | numero | rocce/oggetti diversi registrati |
| `riepilogo.letture` | numero | letture in tutto (la stessa roccia riletta conta una volta nei rilevamenti) |
| `riepilogo.certi` | numero | rilevamenti con un solo materiale possibile (`certo` o `probabile`) |
| `riepilogo.daVerificare` | numero | rilevamenti `ambiguo` o `sconosciuto` (non entrano nei totali) |
| `riepilogo.materiali[]` | elenco | un elemento per materiale, dal più abbondante: `rocce` = rilevamenti, `quantita` = somma delle quantità |
| `rilevamenti[]` | elenco | uno per roccia/oggetto, in ordine di tempo |

Ogni rilevamento:

| Campo | Quando c'è | Significato |
|---|---|---|
| `ora` | sempre | ora della prima lettura di quella roccia (ISO 8601, locale con fuso) |
| `segnale` | sempre | la signature letta dallo scanner |
| `esito` | sempre | `certo` (un solo materiale possibile), `probabile` (uno solo, ma la lettura è stata corretta), `ambiguo` (più materiali possibili: vedi `alternative`), `sconosciuto` (nessun materiale della tabella) |
| `materiale` | `certo`, `probabile` | nome del materiale nel gioco, in inglese (`Copper`, `Quantanium`) |
| `nome` | `certo`, `probabile` | nome come nella tabella di Iris (può avere la traduzione: `Copper (Rame)`) |
| `quantita` | `certo`, `probabile` | rocce (minerale), pezzi (salvage) o depositi (roc/fps) |
| `tipo` | `certo`, `probabile` | `minerale`, `salvage`, `roc` (depositi per ROC), `fps` (depositi a mano) |
| `rarita` | se nota | `comune`, `non comune`, `raro`, `epico`, `leggendario` |
| `alternative[]` | `ambiguo` | i materiali possibili, ognuno con `materiale`, `nome`, `quantita`, `tipo` |
| `origine` | sempre | come è stata letta: `tasto`, `continua`, `voce`, `manuale` (tastierino), `barra` (barra rapida) |
| `letture` | sempre | quante volte quella roccia è stata letta (rilette entro 45 secondi = stessa roccia) |

## Per il sito

- Riconoscere il file da `formato` e accettare `versione` ≥ 1 (ignorare i campi che non si conoscono).
- Per statistiche "cosa si trova dove": `luogo` + `riepilogo.materiali`; per la qualità: `qualitaMedia`.
- I rilevamenti `ambiguo` e `sconosciuto` sono già esclusi dai totali: si possono mostrare a parte.
- I nomi da usare come chiave sono quelli di `materiale` (inglese, come nel gioco).
