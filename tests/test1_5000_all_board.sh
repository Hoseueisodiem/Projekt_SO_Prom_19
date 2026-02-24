#!/bin/bash
# TEST 1: 5000 pasazerow, wszyscy powinni wejsc na prom
# - 3 promy po 500 (1500 naraz), brak VIP, bagaz 1kg, brak niebezpiecznych
# - inspekcja skrocona do 10ms, podroze skrocone (3s)
# Oczekiwany wynik: 5000 "BOARDED ferry"

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
TMP_DIR="/tmp/prom_test1_$$"
LOG_FILE="$PROJECT_DIR/simulation_test.log"

echo "=== TEST 1: 5000 pasazerow - wszyscy wchodza ==="
echo "Katalog tymczasowy: $TMP_DIR"

# Kopiuj zrodla (TMP_DIR nie istnieje - cp -r tworzy go jako kopie src/)
cp -r "$PROJECT_DIR/src" "$TMP_DIR"

# === security.h ===
# 3 promy x 500 = 1500 miejsc naraz
sed -i 's/#define FERRY_CAPACITY 100/#define FERRY_CAPACITY 500/' "$TMP_DIR/security.h"
# brak niebezpiecznych przedmiotow
sed -i 's/#define DANGEROUS_ITEM_CHANCE 15/#define DANGEROUS_ITEM_CHANCE 0/' "$TMP_DIR/security.h"
# bagaz nigdy nie blokuje
sed -i 's/#define MAX_BAGGAGE 20/#define MAX_BAGGAGE 9999/' "$TMP_DIR/security.h"
# krotszy czas odplywania
sed -i 's/#define DEPARTURE_TIME 8/#define DEPARTURE_TIME 3/' "$TMP_DIR/security.h"
# krotszy rejs
sed -i 's/#define TRAVEL_TIME 10/#define TRAVEL_TIME 3/' "$TMP_DIR/security.h"
# prawie natychmiastowa inspekcja (10ms zamiast 300ms)
sed -i 's/#define INSPECTION_TIME_MS 300/#define INSPECTION_TIME_MS 10/' "$TMP_DIR/security.h"

# === captain_port.cpp: limity bagazu -> 9999 (bagaz nigdy nie blokuje) ===
sed -i "s/baggage_limit = 25/baggage_limit = 9999/" "$TMP_DIR/captain_port.cpp"
sed -i "s/baggage_limit = 20/baggage_limit = 9999/" "$TMP_DIR/captain_port.cpp"
sed -i "s/baggage_limit = 30/baggage_limit = 9999/" "$TMP_DIR/captain_port.cpp"
# szybszy polling glownej petli kapitan_port (100ms -> 5ms)
sed -i 's/usleep(100000);/usleep(5000);/' "$TMP_DIR/captain_port.cpp"
# szybki polling przy zamykaniu portu
sed -i 's/sleep(1);/usleep(10000);/g' "$TMP_DIR/captain_port.cpp"

# === main.cpp ===
# 5000 pasazerow
sed -i 's/int num_passengers = 1000/int num_passengers = 5000/' "$TMP_DIR/main.cpp"
# szybkie generowanie pasazerow (100us odstep zamiast 10ms)
sed -i 's/usleep(10000); \/\/odstep/usleep(100); \/\/odstep/' "$TMP_DIR/main.cpp"
# port otwarty przez 120s (4 kursy x 3 promy ~ 30s, bezpieczny bufor)
sed -i 's/sleep(200)/sleep(120)/' "$TMP_DIR/main.cpp"

# === passenger.cpp: brak VIP, bagaz 1kg, szybszy polling ===
sed -i 's/bool vip    = (rand() % 5 == 0)/bool vip    = false/' "$TMP_DIR/passenger.cpp"
sed -i 's/int baggage = rand() % 40 + 1/int baggage = 1/' "$TMP_DIR/passenger.cpp"
# szybszy polling boardingu i trapu (1s -> 10ms)
sed -i 's/sleep(1);/usleep(10000);/g' "$TMP_DIR/passenger.cpp"

# === security_station.cpp: przyspieszenie ===
# szybsza glowna petla stanowiska (50ms -> 1ms)
sed -i 's/usleep(50000);/usleep(1000);/' "$TMP_DIR/security_station.cpp"
# szybsze retry looopy inicjalizacji (100ms -> 1ms)
sed -i 's/usleep(100000);/usleep(1000);/g' "$TMP_DIR/security_station.cpp"

# === captain_ferry.cpp: szybszy polling ===
sed -i 's/sleep(1);/usleep(10000);/g' "$TMP_DIR/captain_ferry.cpp"

# Kompilacja
echo "Kompilacja..."
g++ -o "$TMP_DIR/simulation_test" "$TMP_DIR"/*.cpp
echo "Kompilacja OK"

# Uruchomienie
ln -sf "$LOG_FILE" "$TMP_DIR/simulation.log"
> "$LOG_FILE"
echo "Symulacja 5000 pasazerow (log: $LOG_FILE)"
cd "$TMP_DIR"
"$TMP_DIR/simulation_test"
cd "$PROJECT_DIR"

# Weryfikacja
echo ""
echo "=== WYNIKI ==="
BOARDED=$(grep -c "BOARDED ferry" "$LOG_FILE" || true)
echo "Liczba BOARDED ferry: $BOARDED / 5000"

if [ "$BOARDED" -eq 5000 ]; then
    echo "WYNIK: PASS - Wszyscy 5000 pasazerow weszli na prom"
else
    echo "WYNIK: FAIL - Oczekiwano 5000, uzyskano $BOARDED"
    REJECTED=$(grep -c "REJECTED" "$LOG_FILE" || true)
    DANGEROUS=$(grep -c "DANGEROUS ITEM found on passenger" "$LOG_FILE" || true)
    echo "  Odrzuconych: $REJECTED"
    echo "  Niebezpieczne przedmioty (powinno byc 0): $DANGEROUS"
fi

# Sprzatanie
rm -rf "$TMP_DIR"
echo ""
echo "Log zapisany w: $LOG_FILE"
