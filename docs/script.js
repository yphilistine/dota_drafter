var i18n = {
  ru: {
    heroSubtitle: "Автоматический помощник драфта с ML-рекомендациями, распознаванием героев и live-overlay",
    downloadBtn: "Скачать установщик",
    screenshotAlt: "Скриншот приложения",
    featuresTitle: "Возможности",
    feat1Title: "ML-предсказания",
    feat1Desc: "CatBoost модель предсказывает вероятность победы для каждого возможного пика",
    feat2Title: "Захват и распознавание",
    feat2Desc: "Автоматический захват портретов героев и определение позиций из HUD игры каждые 500 мс",
    feat3Title: "Статистика игрока",
    feat3Desc: "Персональная статистика героев из OpenDota и STRATZ",
    feat4Title: "Низкое потребление",
    feat4Desc: "~80 MB RAM, ~3% CPU в ожидании, ~10-15% во время драфта",
    howTitle: "Как это работает",
    step1Title: "Установите",
    step1Desc: "Скачайте и запустите установщик. Права администратора не нужны",
    step2Title: "Введите Friend ID",
    step2Desc: "Укажите свой числовой ID из профиля Dota 2",
    step3Title: "Запустите Dota 2",
    step3Desc: "Начните матч — приложение обнаружит игру автоматически",
    step4Title: "Получайте рекомендации",
    step4Desc: "ML-модель предложит лучшие пики в реальном времени",
    reqTitle: "Системные требования",
    req1: "Windows 10 или новее",
    req2: "Dota 2",
    req3: "Интернет-соединение",
    req4: "~80 MB RAM",
    dlTitle: "Скачать",
    dlSubtitle: "",
    dlBtn: "Скачать dota_drafter_setup.exe",
    inst1: "Скачайте и запустите установщик",
    inst2: "Приложение установится в %LOCALAPPDATA%\\Dota_Drafter",
    inst3: "GSI-конфигурация установится автоматически",
    inst4: "Запустите через ярлык на рабочем столе",
    footerRepo: "Репозиторий",
    footerVersion: "v0.2.1"
  },
  en: {
    heroSubtitle: "Automatic draft assistant with ML recommendations, hero recognition and live overlay",
    downloadBtn: "Download Installer",
    screenshotAlt: "App screenshot",
    featuresTitle: "Features",
    feat1Title: "ML Predictions",
    feat1Desc: "CatBoost model predicts win probability for every possible pick",
    feat2Title: "Capture and Recognition",
    feat2Desc: "Automatic hero portrait capture and position detection from game HUD every 500 ms",
    feat3Title: "Player Stats",
    feat3Desc: "Personal hero statistics from OpenDota and STRATZ",
    feat4Title: "Low Resource Usage",
    feat4Desc: "~80 MB RAM, ~3% CPU idle, ~10-15% during draft",
    howTitle: "How It Works",
    step1Title: "Install",
    step1Desc: "Download and run the installer. No admin rights required",
    step2Title: "Enter Friend ID",
    step2Desc: "Enter your numeric ID from your Dota 2 profile",
    step3Title: "Launch Dota 2",
    step3Desc: "Start a match -- the app detects the game automatically",
    step4Title: "Get Recommendations",
    step4Desc: "ML model suggests the best picks in real time",
    reqTitle: "System Requirements",
    req1: "Windows 10 or later",
    req2: "Dota 2",
    req3: "Internet connection",
    req4: "~80 MB RAM",
    dlTitle: "Download",
    dlSubtitle: "",
    dlBtn: "Download dota_drafter_setup.exe",
    inst1: "Download and run the installer",
    inst2: "Installs to %LOCALAPPDATA%\\Dota_Drafter",
    inst3: "GSI configuration is set up automatically",
    inst4: "Launch from the desktop shortcut",
    footerRepo: "Repository",
    footerVersion: "v0.2.1"
  }
};

var currentLang = "ru";

function setLang(lang) {
  currentLang = lang;
  document.documentElement.lang = lang;
  document.querySelectorAll("[data-i18n]").forEach(function (el) {
    var key = el.getAttribute("data-i18n");
    if (i18n[lang][key]) {
      el.textContent = i18n[lang][key];
    }
  });
  document.getElementById("lang-toggle").textContent =
    lang === "ru" ? "EN" : "RU";
  localStorage.setItem("lang", lang);
}

document.addEventListener("DOMContentLoaded", function () {
  var saved = localStorage.getItem("lang");
  setLang(saved && i18n[saved] ? saved : "ru");

  document.getElementById("lang-toggle").addEventListener("click", function () {
    setLang(currentLang === "ru" ? "en" : "ru");
  });
});
