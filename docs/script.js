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
    feat4Desc: "Потребление ресурсов зависит от устройства. ~80 MB RAM, ~3% CPU в ожидании",
    feat5Title: "Панель Meta Heroes",
    feat5Desc: "Топ-10 популярных героев по позициям на основе живой статистики STRATZ, с винрейтом за последнюю неделю",
    feat6Title: "Overlay-кнопка [D]",
    feat6Desc: "Прозрачная кнопка поверх Dota 2 для быстрого переключения фокуса между игрой и приложением без Alt+Tab",
    howTitle: "Как это работает",
    step1Title: "Установите",
    step1Desc: "Скачайте и запустите установщик. Права администратора не нужны",
    step2Title: "Введите Friend ID",
    step2Desc: "Необязательно: укажите числовой ID из профиля Dota 2 для персональных рекомендаций. Без него драфт и герои всё равно распознаются",
    step3Title: "Запустите Dota 2",
    step3Desc: "Начните матч — приложение обнаружит игру автоматически",
    step4Title: "Получайте рекомендации",
    step4Desc: "ML-модель предложит лучшие пики в реальном времени",
    friendIdTitle: "Где взять Friend ID",
    friendIdSubtitle: "Friend ID можно скопировать прямо в Steam или Dota 2, не покидая клиент",
    friendIdStep1: "Откройте список друзей Steam (в Dota 2 или через оверлей Steam) и нажмите иконку добавления друга рядом со строкой поиска",
    friendIdStep2: "В окне «Add Friend» скопируйте число рядом с «Your Friend ID is» кнопкой «Copy Selected Text» — это и есть ваш Friend ID",
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
    poweredBy: "Данные предоставлены",
    contactLabel: "Контакты",
    footerRepo: "Репозиторий",
    footerVersion: ""
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
    feat4Desc: "Resource usage depends on your device. ~80 MB RAM, ~3% CPU idle",
    feat5Title: "Meta Heroes Panel",
    feat5Desc: "Top 10 popular heroes per position from live STRATZ stats, with win rate for the past week",
    feat6Title: "Overlay Button [D]",
    feat6Desc: "A transparent button over Dota 2 for quick focus switching between the game and the app, no Alt+Tab needed",
    howTitle: "How It Works",
    step1Title: "Install",
    step1Desc: "Download and run the installer. No admin rights required",
    step2Title: "Enter Friend ID",
    step2Desc: "Optional: enter your numeric ID from your Dota 2 profile for personalized recommendations. Draft and hero recognition work without it too",
    step3Title: "Launch Dota 2",
    step3Desc: "Start a match -- the app detects the game automatically",
    step4Title: "Get Recommendations",
    step4Desc: "ML model suggests the best picks in real time",
    friendIdTitle: "Where to Find Your Friend ID",
    friendIdSubtitle: "You can copy your Friend ID right from Steam or Dota 2, no need to leave the client",
    friendIdStep1: "Open your Steam friends list (in Dota 2 or via the Steam overlay) and click the Add Friend icon next to the search bar",
    friendIdStep2: "In the \"Add Friend\" window, copy the number next to \"Your Friend ID is\" using the \"Copy Selected Text\" button -- that's your Friend ID",
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
    poweredBy: "Powered by",
    contactLabel: "Contact",
    footerRepo: "Repository",
    footerVersion: ""
  }
};

var currentLang = "ru";

var MANIFEST_URL = "https://raw.githubusercontent.com/yphilistine/dota_drafter/main/manifest.json";
var remoteAppVersion = null;

function applyRemoteVersion() {
  if (!remoteAppVersion) return;
  var el = document.getElementById("footer-version");
  if (el) el.textContent = "v" + remoteAppVersion;
}

function fetchManifestVersion() {
  fetch(MANIFEST_URL)
    .then(function (res) { return res.json(); })
    .then(function (data) {
      if (data && data.app && data.app.version) {
        remoteAppVersion = data.app.version;
        applyRemoteVersion();
      }
    })
    .catch(function () { /* keep whatever is already shown */ });
}

function setLang(lang) {
  currentLang = lang;
  document.documentElement.lang = lang;
  document.querySelectorAll("[data-i18n]").forEach(function (el) {
    var key = el.getAttribute("data-i18n");
    if (i18n[lang][key]) {
      el.textContent = i18n[lang][key];
    }
  });
  document.querySelectorAll("[data-i18n-alt]").forEach(function (el) {
    var key = el.getAttribute("data-i18n-alt");
    if (i18n[lang][key]) {
      el.alt = i18n[lang][key];
    }
  });
  document.getElementById("lang-toggle").textContent =
    lang === "ru" ? "EN" : "RU";
  localStorage.setItem("lang", lang);
  applyRemoteVersion();
}

document.addEventListener("DOMContentLoaded", function () {
  var saved = localStorage.getItem("lang");
  setLang(saved && i18n[saved] ? saved : "ru");

  document.getElementById("lang-toggle").addEventListener("click", function () {
    setLang(currentLang === "ru" ? "en" : "ru");
  });

  fetchManifestVersion();
  initCarousel();
});

function initCarousel() {
  var track = document.querySelector(".carousel-track");
  var slides = document.querySelectorAll(".carousel-slide");
  var dots = document.querySelectorAll(".carousel-dot");
  var prev = document.querySelector(".carousel-prev");
  var next = document.querySelector(".carousel-next");
  if (!track || slides.length === 0) return;

  var current = 0;
  var total = slides.length;

  function goTo(index) {
    current = (index + total) % total;
    track.style.transform = "translateX(-" + (current * 100) + "%)";
    dots.forEach(function (d, i) {
      d.classList.toggle("active", i === current);
    });
  }

  prev.addEventListener("click", function () { goTo(current - 1); });
  next.addEventListener("click", function () { goTo(current + 1); });
  dots.forEach(function (d, i) {
    d.addEventListener("click", function () { goTo(i); });
  });

  var touchStartX = 0;
  var touchDeltaX = 0;

  track.parentElement.addEventListener("touchstart", function (e) {
    touchStartX = e.touches[0].clientX;
    touchDeltaX = 0;
  }, { passive: true });

  track.parentElement.addEventListener("touchmove", function (e) {
    touchDeltaX = e.touches[0].clientX - touchStartX;
  }, { passive: true });

  track.parentElement.addEventListener("touchend", function () {
    if (touchDeltaX < -40) goTo(current + 1);
    else if (touchDeltaX > 40) goTo(current - 1);
  });
}
