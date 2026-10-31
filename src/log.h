#pragma once

#include <string>

// Registro del programma: %appdata%\Iris\log.txt (il vecchio diventa
// log.old.txt quando supera 1 MB). Serve a capire cosa è successo quando
// qualcosa non va: ogni riga ha data, ora e cosa stava facendo il programma.
// Thread-safe: lo usano tutti i thread (ascolto, modello, voce, finestra).
namespace Log {
    void info(const std::string& message);
    void warn(const std::string& message);
    void error(const std::string& message);
}
