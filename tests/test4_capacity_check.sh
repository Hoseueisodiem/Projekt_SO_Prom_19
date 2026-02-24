#!/bin/bash
# TEST 4: 500 pasazerow, pojemnosc promu = 10
# Sprawdzenie czy prom NIGDY nie przekracza pojemnosci
# Weryfikacja: zadne "BOARDED" nie ma onboard > capacity (np. 11/10)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
TMP_DIR="/tmp/prom_test4_$$"
LOG_FILE="$PROJECT_DIR/simulation_test.log"

echo "=== TEST 4: Pojemnosc promu nigdy nie przekroczona ==="
echo "Katalog tymczasowy: $TMP_DIR"

# Kopiuj zrodla
cp -r "$PROJECT_DIR/src" "$TMP_DIR"

# Modyfikacje parametrow
# security.h: FERRY_CAPACITY 100 -> 10 (mala pojemnosc)
sed -i 's/#define FERRY_CAPACITY 100/#define FERRY_CAPACITY 10/' "$TMP_DIR/security.h"
# security.h: brak niebezpiecznych
sed -i 's/#define DANGEROUS_ITEM_CHANCE 15/#define DANGEROUS_ITEM_CHANCE 0/' "$TMP_DIR/security.h"
# security.h: bagaz bez limitu
sed -i 's/#define MAX_BAGGAGE 20/#define MAX_BAGGAGE 9999/' "$TMP_DIR/security.h"

# captain_port.cpp: baggage_limit -> 9999
sed -i "s/baggage_limit = 25/baggage_limit = 9999/" "$TMP_DIR/captain_port.cpp"
sed -i "s/baggage_limit = 20/baggage_limit = 9999/" "$TMP_DIR/captain_port.cpp"
sed -i "s/baggage_limit = 30/baggage_limit = 9999/" "$TMP_DIR/captain_port.cpp"

# main.cpp: 500 pasazerow
sed -i 's/int num_passengers = 1000/int num_passengers = 500/' "$TMP_DIR/main.cpp"
# main.cpp: szybsze generowanie
sed -i 's/usleep(10000);.*odstep/usleep(100); \/\/odstep/' "$TMP_DIR/main.cpp"
# main.cpp: krotszy czas
sed -i 's/sleep(200)/sleep(300)/' "$TMP_DIR/main.cpp"

# passenger.cpp: bagaz lekki
sed -i 's/int baggage = rand() % 40 + 1/int baggage = 1/' "$TMP_DIR/passenger.cpp"
# passenger.cpp: brak VIP
sed -i 's/bool vip    = (rand() % 5 == 0)/bool vip    = false/' "$TMP_DIR/passenger.cpp"

# Przyspieszenie
# security_station.cpp: security check instant (zamien wszystkie usleep 100ms na 1ms)
sed -i 's/usleep(100000)/usleep(1000)/g' "$TMP_DIR/security_station.cpp"
sed -i 's/sleep(1);/usleep(10000);/g' "$TMP_DIR/passenger.cpp"
sed -i 's/#define DEPARTURE_TIME 8/#define DEPARTURE_TIME 2/' "$TMP_DIR/security.h"
sed -i 's/#define TRAVEL_TIME 10/#define TRAVEL_TIME 1/' "$TMP_DIR/security.h"
sed -i 's/sleep(1);/usleep(10000);/g' "$TMP_DIR/captain_ferry.cpp"
sed -i 's/sleep(1);/usleep(10000);/g' "$TMP_DIR/captain_port.cpp"

# Kompilacja
echo "Kompilacja..."
g++ -o "$TMP_DIR/simulation_test" "$TMP_DIR"/*.cpp
echo "Kompilacja OK"

# Uruchomienie - log zapisuje sie na biezaco do simulation_test.log
ln -sf "$LOG_FILE" "$TMP_DIR/simulation.log"
echo "Symulacja 500 pasazerow, pojemnosc promu 10 (log: $LOG_FILE)"
cd "$TMP_DIR"
"$TMP_DIR/simulation_test"
cd "$PROJECT_DIR"

# Weryfikacja
echo ""
echo "=== WYNIKI ==="

BOARDED=$(grep -c "BOARDED ferry" "$LOG_FILE" || true)
echo "BOARDED ferry: $BOARDED / 500"

# Sprawdz czy jakikolwiek wpis BOARDED ma onboard > capacity (np. 11/10, 12/10 itd.)
# Format: "BOARDED ferry X (... Y/10 onboard, ...)"
# Wyciagamy Y/10 i sprawdzamy czy Y > 10
OVERFLOW=$(grep "BOARDED ferry" "$LOG_FILE" | grep -oP '\d+/10 onboard' | while read line; do
    ONBOARD=$(echo "$line" | cut -d'/' -f1)
    if [ "$ONBOARD" -gt 10 ]; then
        echo "$line"
    fi
done)

OVERFLOW_COUNT=$(echo "$OVERFLOW" | grep -c '.' 2>/dev/null || true)
if [ -z "$OVERFLOW" ]; then
    OVERFLOW_COUNT=0
fi

echo "Przekroczenia pojemnosci (onboard > 10): $OVERFLOW_COUNT (oczekiwane: 0)"

if [ "$OVERFLOW_COUNT" -gt 0 ]; then
    echo "Przykladowe przekroczenia:"
    echo "$OVERFLOW" | head -5
fi

PASS=true

if [ "$OVERFLOW_COUNT" -gt 0 ]; then
    echo "FAIL: Prom przekroczyl pojemnosc!"
    PASS=false
fi

if [ "$BOARDED" -ne 500 ]; then
    echo "UWAGA: Nie wszyscy pasazerowie weszli ($BOARDED/500)"
fi

if [ "$PASS" = true ]; then
    echo ""
    echo "WYNIK: PASS - Pojemnosc promu nigdy nie przekroczona"
else
    echo ""
    echo "WYNIK: FAIL"
fi

# Sprzatanie
rm -rf "$TMP_DIR"
echo ""
echo "Log zapisany w: $LOG_FILE"
