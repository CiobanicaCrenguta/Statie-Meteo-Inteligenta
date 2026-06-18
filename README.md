Stație Meteo Inteligentă Autonomă

Acest repository conține implementarea unei stații meteorologice inteligente, realizată ca proiect de licență. Sistemul colectează date meteo locale cu ajutorul unui nod ESP32, transmite măsurătorile prin MQTT către un server Raspberry Pi, salvează datele într-o bază SQLite, le afișează într-o interfață web și generează prognoze pe termen scurt folosind modele LightGBM.

Funcționalități principale
colectarea parametrilor meteorologici locali;
măsurarea temperaturii, umidității, presiunii, radiației UV, vitezei vântului, rafalelor, direcției vântului și precipitațiilor;
transmiterea datelor de la ESP32 către Raspberry Pi prin MQTT;
salvarea măsurătorilor într-o bază de date SQLite;
afișarea datelor într-o interfață web realizată cu Flask și Chart.js;
generarea prognozelor meteo pentru orizonturi de la +3h până la +24h;
rularea automată a serviciilor prin systemd și cron.
Arhitectura sistemului

Sistemul este împărțit în două componente principale:

Nodul ESP32
Citește senzorii meteo, agregă local valorile și transmite periodic un mesaj JSON prin MQTT.
Serverul Raspberry Pi
Rulează brokerul Mosquitto, receptorul MQTT, baza de date SQLite, aplicația Flask și modulul de predicție.

Fluxul general al datelor este:

Senzori meteo
    ↓
ESP32
    ↓ MQTT
Raspberry Pi / Mosquitto
    ↓
receptor.py
    ↓
SQLite
    ↓
Flask + Chart.js
    ↓
Interfață web

Pentru prognoză:

SQLite
    ↓
predict_forecast.py
    ↓
Agregare orară + feature engineering
    ↓
Modele LightGBM
    ↓
forecast.json
    ↓
Interfață web
Componente hardware
ESP32;
Raspberry Pi;
senzor BME280 pentru temperatură, umiditate și presiune;
senzor UV ML8511;
anemometru Misol;
giruetă Misol;
pluviometru tipping bucket;
panou solar și modul UPS-15W;
cutie IP67 pentru protecția plăcii;
carcasă de tip Stevenson screen pentru senzorul BME280.
Tehnologii utilizate
ESP32 / Arduino IDE;
MQTT;
Mosquitto;
Python;
Flask;
SQLite;
Chart.js;
LightGBM;
Google Colab;
systemd;
cron.
Structura proiectului
meteo_app/
│
├── app.py                  # aplicația Flask
├── receptor.py             # receptor MQTT pentru salvarea datelor
├── predict_forecast.py     # script pentru generarea prognozei
├── baza_meteo.db           # baza de date SQLite
├── forecast.json           # fișierul cu ultima prognoză generată
├── metadata.json           # configurația modelelor ML
│
├── models/                 # modelele LightGBM exportate
│   ├── *.txt
│
├── templates/
│   └── index.html          # interfața web
│
├── static/
│   └── ...                 # fișiere CSS/JS, inclusiv Chart.js
│
└── esp32/
    └── weather_node.ino    # codul pentru nodul ESP32
Formatul mesajului MQTT

ESP32 publică datele pe topicul:

meteo/senzori

Exemplu de payload JSON:

{
  "temperatura": 22.5,
  "umiditate": 45,
  "presiune": 1014.2,
  "viteza_vant": 12.5,
  "rafala": 18.0,
  "uv": 2.1,
  "precipitatii": 0.0,
  "directie_vant": 180
}
API-uri Flask

Aplicația Flask expune următoarele rute:

Rută	Descriere
/	afișează interfața web
/api/current	returnează ultima măsurătoare salvată
/api/history	returnează istoricul recent al măsurătorilor
/api/forecast	returnează ultima prognoză generată
Modulul de predicție

Modelele de predicție au fost antrenate în Google Colab folosind date meteorologice istorice. Pentru antrenare au fost construite caracteristici de tip:

lag;
trend;
rolling window;
codare ciclică a timpului;
codare sinus/cosinus pentru direcția vântului.

Modelele LightGBM sunt exportate și folosite pe Raspberry Pi doar pentru inferență. Scriptul predict_forecast.py citește datele din SQLite, le agregă la nivel orar, reconstruiește caracteristicile și generează fișierul forecast.json.

Rulare pe Raspberry Pi

Instalare dependențe:

python3 -m venv mediu_meteo
source mediu_meteo/bin/activate
pip install flask pandas numpy paho-mqtt lightgbm scikit-learn

Pornire receptor MQTT:

python receptor.py

Pornire aplicație Flask:

python app.py

Rulare predicție:

python predict_forecast.py
Servicii automate

Pentru funcționare continuă, sistemul poate fi configurat cu:

mosquitto.service;
meteo_receptor.service;
meteo_web.service;
cron pentru rularea periodică a scriptului predict_forecast.py.
Observații

Datele sensibile, precum parola Wi-Fi, SSID-ul rețelei sau adresele IP locale, nu trebuie urcate în repository. Acestea trebuie mutate într-un fișier de configurare ignorat prin .gitignore.

Exemplu:

config.local.h
.env
Stadiul proiectului

Proiectul este funcțional și include fluxul complet:

citire senzori → MQTT → SQLite → Flask → interfață web → prognoză
Autor


