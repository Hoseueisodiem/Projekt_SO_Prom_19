#!/bin/bash
# TEST 5: Stres test kladki (gangway) - GANGWAY_CAPACITY=1 z VIP
# - 1000 pasazerow (~20% VIP), 3 promy x 100 pojemnosci (4 kursy potrzebne)
# - GANGWAY_CAPACITY=1: tylko 1 pasazer naraz na trapie danego promu
# - VIP maja priorytet przy wejsciu na trap (regulani czekaja jesli sa VIP)
# - Maksymalna rywalizacja na semaforze IPC_NOWAIT -> wykrywa deadlock/livelock
# Oczekiwany wynik:
#   1000 BOARDED (brak deadlocka, VIP i regulani wsiadaja)
#   max onboard na promie <= 100 (brak overflow mimo rywalizacji VIP/regular)
#   3 promy zamkniete cleanly (brak zawieszenia)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
TMP_DIR="/tmp/prom_test5_$$"
LOG_FILE="$PROJECT_DIR/simulation_test.log"

echo "=== TEST 5: Stres test kladki (GANGWAY_CAPACITY=1) ==="
echo "Katalog tymczasowy: $TMP_DIR"

# Kopiuj zrodla (TMP_DIR nie istnieje - cp -r tworzy go jako kopie src/)
cp -r "$PROJECT_DIR/src" "$TMP_DIR"

# === security.h ===
# KLUCZOWA ZMIANA: tylko 1 pasazer naraz na trapie danego promu
sed -i 's/#define GANGWAY_CAPACITY 10/#define GANGWAY_CAPACITY 1/' "$TMP_DIR/security.h"
# Standardowe pojemnosci (3 x 100 = 300 na kurs, 4 kursy dla 1000 pasazerow)
# FERRY_CAPACITY zostaje 100 (bez zmian)
# brak niebezpiecznych - testujemy tylko kladke
sed -i 's/#define DANGEROUS_ITEM_CHANCE 15/#define DANGEROUS_ITEM_CHANCE 0/' "$TMP_DIR/security.h"
# bagaz nie blokuje
sed -i 's/#define MAX_BAGGAGE 20/#define MAX_BAGGAGE 9999/' "$TMP_DIR/security.h"
# krotszy czas odplywania
sed -i 's/#define DEPARTURE_TIME 8/#define DEPARTURE_TIME 3/' "$TMP_DIR/security.h"
# krotszy rejs
sed -i 's/#define TRAVEL_TIME 10/#define TRAVEL_TIME 3/' "$TMP_DIR/security.h"
# szybka inspekcja
sed -i 's/#define INSPECTION_TIME_MS 300/#define INSPECTION_TIME_MS 10/' "$TMP_DIR/security.h"

# === captain_port.cpp: limity bagazu -> 9999 ===
sed -i "s/baggage_limit = 25/baggage_limit = 9999/" "$TMP_DIR/captain_port.cpp"
sed -i "s/baggage_limit = 20/baggage_limit = 9999/" "$TMP_DIR/captain_port.cpp"
sed -i "s/baggage_limit = 30/baggage_limit = 9999/" "$TMP_DIR/captain_port.cpp"
# szybszy polling glownej petli kapitan_port (100ms -> 5ms)
sed -i 's/usleep(100000);/usleep(5000);/' "$TMP_DIR/captain_port.cpp"
# szybki polling przy zamykaniu portu
sed -i 's/sleep(1);/usleep(10000);/g' "$TMP_DIR/captain_port.cpp"

# === main.cpp: 1000 pasazerow (domyslna wartosc), szybkie generowanie ===
sed -i 's/usleep(10000); \/\/odstep/usleep(100); \/\/odstep/' "$TMP_DIR/main.cpp"
# port otwarty przez 120s (4 kursy ~ 30s + bufor)
sed -i 's/sleep(200)/sleep(120)/' "$TMP_DIR/main.cpp"

# === passenger.cpp: VIP losowy (~20%), bagaz 1kg, szybszy polling ===
# vip pozostaje losowe (rand() % 5 == 0) - nie zmieniamy
sed -i 's/int baggage = rand() % 40 + 1/int baggage = 1/' "$TMP_DIR/passenger.cpp"
# szybszy polling boardingu i trapu (1s -> 10ms)
sed -i 's/sleep(1);/usleep(10000);/g' "$TMP_DIR/passenger.cpp"

# === security_station.cpp: przyspieszenie ===
# szybsza glowna petla stanowiska (50ms -> 1ms)
sed -i 's/usleep(50000);/usleep(1000);/' "$TMP_DIR/security_station.cpp"
# szybkie retry looopy inicjalizacji (100ms -> 1ms)
sed -i 's/usleep(100000);/usleep(1000);/g' "$TMP_DIR/security_station.cpp"

# === captain_ferry.cpp: szybszy polling ===
sed -i 's/sleep(1);/usleep(10000);/g' "$TMP_DIR/captain_ferry.cpp"

# Kompilacja
echo "Kompilacja..."
g++ -o "$TMP_DIR/simulation_test" "$TMP_DIR"/*.cpp
echo "Kompilacja OK"
echo ""
echo "Konfiguracja: 1000 pasazerow (~20% VIP), 3 promy x 100, GANGWAY_CAPACITY=1"
echo "Oczekiwane kursy: ~4 (300+300+300+100)"

# Uruchomienie
ln -sf "$LOG_FILE" "$TMP_DIR/simulation.log"
> "$LOG_FILE"
echo "Symulacja (log: $LOG_FILE)"
cd "$TMP_DIR"
"$TMP_DIR/simulation_test"
cd "$PROJECT_DIR"

# Weryfikacja
echo ""
echo "=== WYNIKI ==="

BOARDED=$(grep -c "BOARDED ferry" "$LOG_FILE" || true)
VIP_BOARDED=$(grep -c "(VIP) BOARDED ferry" "$LOG_FILE" || true)
REG_BOARDED=$((BOARDED - VIP_BOARDED))
echo "BOARDED lacznie:        $BOARDED / 1000  (VIP: $VIP_BOARDED, regular: $REG_BOARDED)"

# Sprawdz maksymalne zapelnienie promu (w logu: "A/100 onboard")
MAX_ONBOARD=$(grep "BOARDED ferry" "$LOG_FILE" | grep -oP '[0-9]+/100 onboard' | cut -d'/' -f1 | sort -n | tail -1 || true)
echo "Max onboard na promie:  $MAX_ONBOARD / 100 (oczekiwane: <= 100)"

# Czyste zamkniecie promow
FERRY_SHUTDOWN=$(grep -c "\[CAPTAIN FERRY.*\] Port closed.*Shutting down" "$LOG_FILE" || true)
echo "Promy zamkniete:        $FERRY_SHUTDOWN / 3"

# Stanowiska kontroli
STATION_SHUTDOWN=$(grep -c "Shutdown complete" "$LOG_FILE" || true)
echo "Stanowiska zamkniete:   $STATION_SHUTDOWN / 3"

# Informacyjne: race condition na trapie (powinien wystepowac, dowodzi ze kod go obsluguje)
GANGWAY_RETRY=$(grep -c "Ferry became full while on gangway" "$LOG_FILE" || true)
echo "Retry na trapie (edge): $GANGWAY_RETRY (przy GANGWAY_CAPACITY=1 oczekiwane: 0 - tylko 1 pasazer naraz na trapie)"

PASS=true

if [ "$BOARDED" -ne 1000 ]; then
    echo "FAIL: Oczekiwano 1000 BOARDED, uzyskano $BOARDED"
    REJECTED=$(grep -c "REJECTED" "$LOG_FILE" || true)
    echo "  Odrzuconych: $REJECTED"
    PASS=false
fi

if [ -n "$MAX_ONBOARD" ] && [ "$MAX_ONBOARD" -gt 100 ]; then
    echo "FAIL: Overflow - prom przewiozl $MAX_ONBOARD/100 pasazerow naraz"
    PASS=false
fi

if [ "$FERRY_SHUTDOWN" -ne 3 ]; then
    echo "FAIL: Nie wszystkie promy zamknely sie ($FERRY_SHUTDOWN/3) - mozliwe zawieszenie"
    PASS=false
fi

if [ "$PASS" = true ]; then
    echo ""
    echo "WYNIK: PASS - Brak deadlocka/liveloca na kladce przy GANGWAY_CAPACITY=1 (VIP+regular)"
else
    echo ""
    echo "WYNIK: FAIL"
fi

# Sprzatanie
rm -rf "$TMP_DIR"
echo ""
echo "Log zapisany w: $LOG_FILE"
