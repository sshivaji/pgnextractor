# pgnextractor

This repo helps extract and offset information from PGN files and outputs it to a JSON file (more output formats can be added later). While libraries like python-chess can also help do this, this solution is much faster due to it being written in C++. It is based off of mcostalba/chess_db.

Once you have the header information, you can use this to index header based searches in the database. Its also possible to use this in conjunction with https://github.com/mcostalba/chess_db for combined header and position search.

1. To build, go to parser and execute "make build ARCH=x86-64 (or whatever your architecture is)"
2. sudo make install to make a system binary

To run:

1. Execute `pgnextractor headers <pgn file>` 

This will generate a `<pgn file>.headers.json` file that contains all the header and offset information.
