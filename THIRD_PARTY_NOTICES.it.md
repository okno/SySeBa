# Informazioni sulle dipendenze di terze parti

[English authoritative notices](THIRD_PARTY_NOTICES.md) |
[Indice documentazione](docs/README.it.md)

Questa pagina è un riepilogo informativo. Testi di licenza, copyright,
versioni e URL autorevoli restano quelli contenuti in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) e nei file `LICENSE`
vendorizzati.

| Componente | Uso | Licenza |
|---|---|---|
| SQLite | Database audit incorporato | Pubblico dominio |
| cJSON | Parsing/generazione JSON | MIT |
| CivetWeb | Server HTTP incorporato | MIT |

Gli strumenti di build CMake/CPack, Ninja, Zig, MinGW-w64, NSIS e
`libdmg-hfsplus` non vengono incorporati tutti nel runtime finale. Chi
redistribuisce tali strumenti deve consultare le rispettive licenze upstream.

SySeBa è distribuito con licenza MIT. Una traduzione non sostituisce mai il
testo legale originale.
