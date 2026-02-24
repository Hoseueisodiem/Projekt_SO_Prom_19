#!/bin/bash
# TEST 3: 5000 pasazerow z bagaze 50kg - wszyscy powinni byc odrzuceni
# Max limit bagazu promow to 30kg, wiec 50kg = za ciezki
# Oczekiwany wynik: 5000 REJECTED, 0 BOARDED

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
TMP_DIR="/tmp/prom_test3_$$"
LOG_FILE="$PROJECT_DIR/simulation_test.log"

echo "=== TEST 3: 5000 pasazerow z za ciezkim bagazem ==="
echo "Katalog tymczasowy: $TMP_DIR"

# Kopiuj zrodla
cp -r "$PROJECT_DIR/src" "$TMP_DIR"

# Modyfikacje parametrow
# main.cpp: num_passengers 1000 -> 5000
sed -i 's/int num_passengers = 1000/int num_passengers = 5000/' "$TMP_DIR/main.cpp"
# main.cpp: szybsze generowanie
sed -i 's/usleep(10000);.*odstep/usleep(100); \/\/odstep/' "$TMP_DIR/main.cpp"
# main.cpp: krotszy czas (pasazerowie szybko odpadna)
sed -i 's/sleep(200)/sleep(120)/' "$TMP_DIR/main.cpp"

# passenger.cpp: bagaz = 50 kg (za ciezki dla wszystkich promow - max limit 30kg)
sed -i 's/int baggage = rand() % 40 + 1/int baggage = 50/' "$TMP_DIR/passenger.cpp"

# Przyspieszenie
sed -i 's/sleep(1);/usleep(10000);/g' "$TMP_DIR/passenger.cpp"
sed -i 's/sleep(1);/usleep(10000);/g' "$TMP_DIR/captain_ferry.cpp"
sed -i 's/sleep(1);/usleep(10000);/g' "$TMP_DIR/captain_port.cpp"
sed -i 's/#define DEPARTURE_TIME 8/#define DEPARTURE_TIME 2/' "$TMP_DIR/security.h"
sed -i 's/#define TRAVEL_TIME 10/#define TRAVEL_TIME 1/' "$TMP_DIR/security.h"

# Kompilacja
echo "Kompilacja..."
g++ -o "$TMP_DIR/simulation_test" "$TMP_DIR"/*.cpp
echo "Kompilacja OK"

# Uruchomienie - log zapisuje sie na biezaco do simulation_test.log
ln -sf "$LOG_FILE" "$TMP_DIR/simulation.log"
echo "Symulacja 5000 pasazerow wszyscy ciezki bagaz (log: $LOG_FILE)"
cd "$TMP_DIR"
"$TMP_DIR/simulation_test"
cd "$PROJECT_DIR"

# Weryfikacja
echo ""
echo "=== WYNIKI ==="

REJECTED=$(grep -c "REJECTED (baggage too heavy)" "$LOG_FILE" || true)
echo "REJECTED (baggage too heavy): $REJECTED (oczekiwane: 5000)"

BOARDED=$(grep -c "BOARDED ferry" "$LOG_FILE" || true)
echo "BOARDED ferry: $BOARDED (oczekiwane: 0)"

PASS=true

if [ "$REJECTED" -ne 5000 ]; then
    echo "FAIL: Oczekiwano 5000 odrzuconych, uzyskano $REJECTED"
    PASS=false
fi

if [ "$BOARDED" -ne 0 ]; then
    echo "FAIL: Nikt nie powinien wejsc na prom, a weszlo $BOARDED"
    PASS=false
fi

if [ "$PASS" = true ]; then
    echo ""
    echo "WYNIK: PASS - Wszyscy 5000 odrzuceni za ciezki bagaz"
else
    echo ""
    echo "WYNIK: FAIL"
fi

# Sprzatanie
rm -rf "$TMP_DIR"
echo ""
echo "Log zapisany w: $LOG_FILE"
