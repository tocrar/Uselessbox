function autoRefresh(){$(".info").load("info_box.html"),setTimeout(function(){$(".switch").load("switch_box.html")},20)}function loadConfig(){$(".config").load("config_box.html")}function loadSideNav(){$("nav").load("sidenav.html")}setInterval("autoRefresh()",1e3);