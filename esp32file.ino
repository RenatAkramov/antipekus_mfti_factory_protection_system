#include <Keypad.h>

// Определяем размеры клавиатуры
const byte ROWS = 4; // 4 строки
const byte COLS = 3; // 3 столбца

// Массив символов на клавиатуре
char keys[ROWS][COLS] = {
  {'1','2','3'},
  {'4','5','6'},
  {'7','8','9'},
  {'*','0','#'}
};

// Пины для подключения строк клавиатуры
byte rowPins[ROWS] = {22, 21, 19, 18};
// Пины для подключения столбцов клавиатуры
byte colPins[COLS] = {5, 4, 15};

// Создаем объект клавиатуры
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// Пароль для проверки
const String password = "1234";
String inputPassword = "";

// Пины для реле и управления замком
const int relayPin = 13; // Пин подключения реле
const int buttonPin = 12; // Пин для тактовой кнопки
const int doorSignalPin = 14; // Пин для сигнала открытия двери
bool isLockOpen = false; // Статус замка

// Таймер для автоматического закрытия
unsigned long lockOpenTime = 0;
const unsigned long lockOpenDuration = 5000; // 5 секунд открытия

// Переменные для обработки кнопки
bool lastButtonState = HIGH;
bool buttonState = HIGH;
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 50;

// Переменные для обработки сигнала на пине 21
bool lastDoorSignalState = LOW;
bool doorSignalState = LOW;
unsigned long lastDoorDebounceTime = 0;
const unsigned long doorDebounceDelay = 50;

void setup() {
  Serial.begin(115200);
  
  // Настраиваем пин реле как выход
  pinMode(relayPin, OUTPUT);
  
  // Настраиваем пин кнопки как вход с подтяжкой к питанию
  pinMode(buttonPin, INPUT_PULLUP);
  
  // Настраиваем пин сигнала двери как вход
  pinMode(doorSignalPin, INPUT);
  
  // Закрываем замок при запуске (реле выключено)
  digitalWrite(relayPin, LOW);
  
  Serial.println("Введите пароль и нажмите # для проверки");
  Serial.println("Нажмите * для очистки ввода");
  Serial.println("Используйте тактовую кнопку для открытия двери");
  Serial.println("Сигнал на пине 21 также открывает дверь на 5 секунд");
}

void loop() {
  // Проверяем состояние кнопки
  checkButton();
  
  // Проверяем состояние сигнала на пине 21
  checkDoorSignal();
  
  // Обрабатываем ввод с клавиатуры
  char key = keypad.getKey();
  
  if (key) {
    Serial.print(key);
    
    if (key == '#') {
      // Проверяем пароль при нажатии #
      if (inputPassword == password) {
        Serial.println("\nПравильный пароль! Доступ разрешен.");
        openLock(); // Открываем замок
      } else {
        Serial.println("\nНеверный пароль! Попробуйте снова.");
      }
      inputPassword = ""; // Сбрасываем ввод
    }
    else if (key == '*') {
      // Очищаем ввод при нажатии *
      inputPassword = "";
      Serial.println("\nВвод очищен");
    }
    else {
      // Добавляем символ к вводимому паролю
      inputPassword += key;
    }
  }
  
  // Проверяем таймер автоматического закрытия
  if (isLockOpen && millis() - lockOpenTime >= lockOpenDuration) {
    closeLock(); // Закрываем замок по истечении времени
  }
}

// Функция проверки состояния кнопки с антидребезгом
void checkButton() {
  bool reading = digitalRead(buttonPin);
  
  // Если состояние изменилось
  if (reading != lastButtonState) {
    lastDebounceTime = millis();
  }
  
  // Если прошло достаточно времени для устранения дребезга
  if ((millis() - lastDebounceTime) > debounceDelay) {
    // Если состояние устойчиво изменилось
    if (reading != buttonState) {
      buttonState = reading;
      
      // Если кнопка нажата (LOW, так как используется INPUT_PULLUP)
      if (buttonState == LOW) {
        Serial.println("\nКнопка нажата - открываем дверь");
        openLock(); // Открываем замок при нажатии кнопки
      }
    }
  }
  
  lastButtonState = reading;
}

// Функция проверки сигнала на пине 21
void checkDoorSignal() {
  bool reading = digitalRead(doorSignalPin);
  
  // Если состояние изменилось
  if (reading != lastDoorSignalState) {
    lastDoorDebounceTime = millis();
  }
  
  // Если прошло достаточно времени для устранения дребезга
  if ((millis() - lastDoorDebounceTime) > doorDebounceDelay) {
    // Если состояние устойчиво изменилось
    if (reading != doorSignalState) {
      doorSignalState = reading;
      
      // Если на пине 21 появился сигнал HIGH
      if (doorSignalState == HIGH) {
        Serial.println("\nСигнал на пине 21 - открываем дверь");
        openLock(); // Открываем замок при поступлении сигнала
      }
      // При сигнале LOW ничего не делаем
    }
  }
  
  lastDoorSignalState = reading;
}

// Функция открытия замка
void openLock() {
  // Если замок уже открыт, сбрасываем таймер
  if (isLockOpen) {
    lockOpenTime = millis();
    Serial.println("Таймер открытия сброшен");
  } else {
    digitalWrite(relayPin, HIGH); // Включаем реле
    isLockOpen = true;
    lockOpenTime = millis(); // Запоминаем время открытия
    Serial.println("Замок открыт");
  }
}

// Функция закрытия замка
void closeLock() {
  digitalWrite(relayPin, LOW); // Выключаем реле
  isLockOpen = false;
  Serial.println("Замок закрыт");
}
