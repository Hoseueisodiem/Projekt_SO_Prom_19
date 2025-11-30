# Temat 19 – Prom: Opis Projektu i Testy

## 1. Wymagane Procedury

Symulacja zostanie zaimplementowana za pomocą następujących procesów:

* **Kapitan portu**
* **Kapitan promu**
* **Pasażerowie**

---

## 2. Opis Symulacji

Symulacja procesu wchodzenia pasażerów na prom, z uwzględnieniem rygorystycznych zasad bezpieczeństwa.

Prom posiada pojemność p pasażerów oraz ustalony dopuszczalny ciężar bagażu podręcznego M_p. Pasażerowie przechodzą kolejno przez następujące etapy, zanim wejdą na pokład:

1.  **Odprawa biletowo-bagażowa:** Pasażerowie z bagażem cięższym niż limit M_p są wycofywani do hali odpraw.
2.  **Kontrola bezpieczeństwa:** Przeprowadzana jest równolegle na 3 stanowiskach. Na każdym stanowisku mogą znajdować się maksymalnie 2 osoby, ale muszą być tej samej płci.
3.  **Poczekalnia:** Pasażerowie oczekują na wejście na pokład.
4.  **Trap:** Wejście na pokład odbywa się przez trap o pojemności K.
5.  **Rejs:** Prom wypływa co określony czas T1 lub na polecenie Kapitana Portu (sygnał 1). Kapitan Promu musi dopilnować, aby na trapie nie było wchodzących pasażerów, a łączna liczba na promie nie przekroczyła P.

---

## 3. Link do Repozytorium GitHub

Adres URL: https://github.com/Hoseueisodiem/Projekt_SO_Prom_19

---

## 4. Przykładowe Testy Jednostkowe

### T1: Weryfikacja kontroli bagażu (Poprawność funkcjonalna)
* **Warunki początkowe:** Symulacja z 10 procesami Pasażerów, gdzie 3 pasażerów ma wylosowaną wagę bagażu o 50% wyższą niż dopuszczalny limit M_p.
* **Przebieg:** Pasażerowie podchodzą do stanowiska odprawy biletowo-bagażowej.
* **Oczekiwany rezultat:** Dokładnie 3 procesy Pasażerów zostają poprawnie zidentyfikowane i wycofane do hali odpraw, a następnie przerywają swój cykl. Pozostałe 7 procesów kontynuuje ruch w kierunku kontroli bezpieczeństwa, co potwierdza, że mechanizm wycofania nie blokuje kolejki.

### T2: Synchronizacja i Poprawność Kontroli Bezpieczeństwa (Unikanie błędu logicznego)
* **Warunki początkowe:** Ustawienie dużej liczby procesów Pasażerów, w tym równa liczba mężczyzn i kobiet. Symulacja odbywa się w warunkach dużego obciążenia.
* **Przebieg:** Pasażerowie jednocześnie podchodzą do stanowisk kontroli bezpieczeństwa (3 stanowiska, maks. 2 osoby).
* **Oczekiwany rezultat:** Na żadnym z 3 stanowisk w danym momencie nie jest obsługiwana para pasażerów różnej płci. Mechanizmy synchronizacji (np. semafory, muteksy i zmienne warunkowe) muszą zapewniać wyłączność dla danej płci, zapobiegając błędowi logicznemu i zakleszczeniu w przypadku braku dostępnych miejsc dla jednej z płci.

### T3: Ograniczenie Pojemności Promu (Zarządzanie zasobami)
* **Warunki początkowe:** Prom o małej pojemności P=5. Symulacja 10 procesów Pasażerów, które przeszły pomyślnie kontrolę i znajdują się w poczekalni. Trap o pojemności K=3.
* **Przebieg:** Pasażerowie wchodzą na pokład przez trap. Kapitan Promu ogłasza gotowość przyjęcia.
* **Oczekiwany rezultat:** Na promie znajduje się dokładnie 5 pasażerów. Pasażerowie, którzy nie zmieścili się na pokład, muszą pozostać w poczekalni. System nie może dopuścić do sytuacji, w której prom przyjmie więcej niż P pasażerów. Weryfikacja, czy proces załadunku zostanie zablokowany, gdy osiągnięta zostanie maksymalna pojemność P.

### T4: Awaryjne Wypłynięcie Promu (Obsługa sygnałów)
* **Warunki początkowe:** Prom na stanowisku załadunku. Rozpoczyna się załadunek pasażerów (np. 3 osoby są na trapie, 2 w poczekalni).
* **Przebieg:** W losowym momencie Kapitan Portu (proces Dyrektor) wysyła sygnał 1 (np. SIGUSR1) do procesu Kapitan Promu.
* **Oczekiwany rezultat:** Kapitan Promu natychmiastowo zamyka załadunek, dopilnowując jednocześnie, by na trapie nie było wchodzących pasażerów, i rozpoczyna rejs przed upływem normalnego czasu T1. Procesy obsługujące sygnały działają poprawnie i nie powodują błędu segmentacji ani zakleszczenia.