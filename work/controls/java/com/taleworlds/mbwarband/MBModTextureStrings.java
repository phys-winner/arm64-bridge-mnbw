package com.taleworlds.mbwarband;

import java.util.Arrays;
import java.util.Locale;

/** Android-owned module conversion and launch text, in every shipped game language. */
public final class MBModTextureStrings {
    public enum Text {
        PREPARING_TITLE, CONVERTING, VERIFYING, SCANNING, STOPPING,
        TIME_AFTER_SCAN, TIME_MEASURING, TIME_ESTIMATE,
        ETC1_ALPHA, TO_CONVERT, DEVICE_FORMAT, ALREADY_READY,
        REPLACE_WARNING, PROCESS_NOTE, CONFIRM_TITLE, CONVERT,
        FAIL_TITLE, FAIL_NOTE, CLOSE,
        INVENTORY_CHANGED, INVALID_TEXTURE, UNSAFE_PATH, LIMIT_EXCEEDED,
        IO_ERROR, CONVERTER_MISSING, GPU_PROBE_FAILED, GPU_UNAVAILABLE,
        GENERIC_ERROR, OPEN_FAILED, OPEN_FAILED_NOTE, LAUNCH_CANCELLED,
        RESUME_NOTE, BASE_GAME
    }

    private static final String[] CODES = {
            "cns", "cnt", "cz", "de", "en", "es", "fr", "hu", "ja", "pl", "ru", "tr"
    };

    // Row order matches CODES; entries match Text order.
    private static final String[][] STRINGS = {
            // cns — 简体中文
            {
                    "准备模组纹理", "转换中", "验证结果", "检查纹理", "将在当前文件处理完后停止…",
                    "总耗时：纹理检查完成后估算", "总耗时：正在测算处理速度…",
                    "完整处理预计：%1$s · 剩余约 %2$s",
                    "ETC1（透明纹理使用 RGBA）", "待转换纹理：", "设备格式：", "已处理：",
                    "模组的纹理文件将被替换。若要恢复原文件，需要原始模组压缩包。",
                    "处理可能需要几分钟。取消后已完成的文件会保留，下次可以继续。验证完成后模组会自动启动。",
                    "准备模组纹理？", "转换",
                    "纹理准备失败", "模组尚未启动。已完成的文件会保留；下次启动时会重新检查。", "关闭",
                    "处理期间模组文件发生变化。请重新运行检查。",
                    "纹理已损坏或格式不受支持。请从原始模组压缩包中恢复该文件。",
                    "模组文件夹中存在链接或指向文件夹外的路径。请安装普通的模组文件副本。",
                    "模组大小或目录结构超出支持范围。",
                    "无法读取或写入模组文件。请检查文件夹权限和可用存储空间。",
                    "此版本无法使用纹理转换器。",
                    "无法检测 GPU 支持的纹理格式。请返回应用并重试。",
                    "图形驱动未确认支持 ASTC 或 ETC1。",
                    "无法处理纹理。请检查可用存储空间和模组文件。",
                    "无法打开模组", "请检查模组文件和文件夹权限。",
                    "模组启动已取消", "已完成的纹理已保存。下次启动时可以继续处理。", "基础游戏"
            },
            // cnt — 繁體中文
            {
                    "準備模組紋理", "轉換中", "驗證結果", "檢查紋理", "將在目前檔案處理完後停止…",
                    "總耗時：紋理檢查完成後估算", "總耗時：正在測量處理速度…",
                    "完整處理預計：%1$s · 剩餘約 %2$s",
                    "ETC1（透明紋理使用 RGBA）", "待轉換紋理：", "裝置格式：", "已處理：",
                    "模組的紋理檔案將被取代。若要還原原檔，需要原始模組壓縮檔。",
                    "處理可能需要數分鐘。取消後已完成的檔案會保留，下次可繼續。驗證完成後模組會自動啟動。",
                    "準備模組紋理？", "轉換",
                    "紋理準備失敗", "模組尚未啟動。已完成的檔案會保留；下次啟動時會重新檢查。", "關閉",
                    "處理期間模組檔案發生變更。請重新執行檢查。",
                    "紋理已損毀或格式不受支援。請從原始模組壓縮檔還原。",
                    "模組資料夾中有連結或指向資料夾外的路徑。請安裝一般的模組檔案副本。",
                    "模組大小或目錄結構超出支援範圍。",
                    "無法讀取或寫入模組檔案。請檢查資料夾權限及可用儲存空間。",
                    "此版本無法使用紋理轉換器。",
                    "無法檢查 GPU 支援的紋理格式。請返回應用程式並重試。",
                    "圖形驅動程式未確認支援 ASTC 或 ETC1。",
                    "無法處理紋理。請檢查可用儲存空間和模組檔案。",
                    "無法開啟模組", "請檢查模組檔案及資料夾權限。",
                    "模組啟動已取消", "已完成的紋理已儲存。下次啟動時可繼續處理。", "主遊戲"
            },
            // cz — Čeština
            {
                    "Příprava textur modulu", "Převod", "Ověřování výsledků", "Kontrola textur", "Zastavení po aktuálním souboru…",
                    "Celkový čas: odhad po kontrole textur", "Celkový čas: měření rychlosti zpracování…",
                    "Celé zpracování: ≈ %1$s · zbývá ≈ %2$s",
                    "ETC1 (RGBA pro průhledné textury)", "Textury k převodu: ", "Formát zařízení: ", "Již hotovo: ",
                    "Soubory textur modulu budou nahrazeny. K obnovení originálů budete potřebovat původní archiv modulu.",
                    "Zpracování může trvat několik minut. Po zrušení zůstanou hotové soubory zachovány a lze pokračovat později. Po ověření se modul automaticky spustí.",
                    "Připravit textury modulu?", "Převést",
                    "Příprava textur se nezdařila", "Modul se nespustil. Hotové soubory zůstanou zachovány; při příštím spuštění budou znovu zkontrolovány.", "Zavřít",
                    "Soubory modulu se během zpracování změnily. Spusťte kontrolu znovu.",
                    "Textura je poškozená nebo má nepodporovaný formát. Obnovte ji z původního archivu modulu.",
                    "Modul obsahuje odkaz nebo cestu mimo svou složku. Nainstalujte běžnou kopii souborů modulu.",
                    "Velikost nebo struktura složek modulu překračuje podporovaný limit.",
                    "Soubory modulu nelze číst nebo zapisovat. Zkontrolujte přístup ke složce a volné místo.",
                    "V této verzi není převodník textur dostupný.",
                    "Podporu formátů na GPU nelze ověřit. Vraťte se do aplikace a zkuste to znovu.",
                    "Grafický ovladač nepotvrdil podporu ASTC ani ETC1.",
                    "Texturu nelze zpracovat. Zkontrolujte volné místo a soubory modulu.",
                    "Modul nelze otevřít", "Zkontrolujte soubory modulu a přístup ke složce.",
                    "Spuštění modulu bylo zrušeno", "Hotové textury byly uloženy. Při příštím spuštění lze pokračovat.", "Základní hra"
            },
            // de — Deutsch
            {
                    "Modultexturen vorbereiten", "Konvertieren", "Ergebnis überprüfen", "Texturen prüfen", "Stopp nach der aktuellen Datei…",
                    "Gesamtzeit: Schätzung nach der Texturprüfung", "Gesamtzeit: Verarbeitungsgeschwindigkeit wird ermittelt…",
                    "Gesamtdauer: ≈ %1$s · verbleibend ≈ %2$s",
                    "ETC1 (RGBA für transparente Texturen)", "Zu konvertierende Texturen: ", "Geräteformat: ", "Bereits fertig: ",
                    "Die Texturdateien des Moduls werden ersetzt. Für die Wiederherstellung der Originale wird das ursprüngliche Modularchiv benötigt.",
                    "Die Verarbeitung kann einige Minuten dauern. Beim Abbrechen bleiben fertige Dateien erhalten; sie kann später fortgesetzt werden. Nach der Prüfung startet das Modul automatisch.",
                    "Modultexturen vorbereiten?", "Konvertieren",
                    "Texturvorbereitung fehlgeschlagen", "Das Modul wurde nicht gestartet. Fertige Dateien bleiben erhalten und werden beim nächsten Start erneut geprüft.", "Schließen",
                    "Moduldateien haben sich während der Verarbeitung geändert. Prüfung erneut starten.",
                    "Eine Textur ist beschädigt oder hat ein nicht unterstütztes Format. Aus dem ursprünglichen Modularchiv wiederherstellen.",
                    "Das Modul enthält einen Link oder einen Pfad außerhalb seines Ordners. Eine normale Kopie der Moduldateien installieren.",
                    "Größe oder Ordnerstruktur des Moduls überschreitet die unterstützte Grenze.",
                    "Moduldateien konnten nicht gelesen oder geschrieben werden. Ordnerzugriff und freien Speicher prüfen.",
                    "Der Texturkonverter ist in dieser Version nicht verfügbar.",
                    "Die GPU-Formatunterstützung konnte nicht geprüft werden. Zur App zurückkehren und erneut starten.",
                    "Der Grafiktreiber hat die Unterstützung für ASTC oder ETC1 nicht bestätigt.",
                    "Eine Textur konnte nicht verarbeitet werden. Freien Speicher und Moduldateien prüfen.",
                    "Modul konnte nicht geöffnet werden", "Moduldateien und Ordnerzugriff prüfen.",
                    "Modulstart abgebrochen", "Fertige Texturen wurden gespeichert. Beim nächsten Start kann die Verarbeitung fortgesetzt werden.", "Grundspiel"
            },
            // en — English
            {
                    "Preparing module textures", "Converting", "Verifying results", "Checking textures", "Stopping after the current file…",
                    "Total time: estimating after texture scan", "Total time: measuring processing speed…",
                    "Total processing: ≈ %1$s · remaining ≈ %2$s",
                    "ETC1 (RGBA for transparent textures)", "Textures to convert: ", "Device format: ", "Already ready: ",
                    "The module's texture files will be replaced. Restoring the originals requires the original module archive.",
                    "Processing may take several minutes. Cancelling keeps completed files so processing can resume. The module will launch automatically after verification.",
                    "Prepare module textures?", "Convert",
                    "Texture preparation failed", "The module has not launched. Completed files are kept; the next launch will check them again.", "Close",
                    "Module files changed during processing. Run the check again.",
                    "A texture is damaged or uses an unsupported format. Restore it from the original module archive.",
                    "The module contains a link or a path outside its folder. Install a regular copy of the module files.",
                    "The module's size or directory structure exceeds the supported limit.",
                    "The module files could not be read or written. Check folder access and free storage.",
                    "The texture converter is unavailable in this build.",
                    "GPU texture support could not be checked. Return to the app and launch it again.",
                    "The graphics driver did not confirm ASTC or ETC1 support.",
                    "A texture could not be processed. Check free storage and the module files.",
                    "Could not open module", "Check the module files and folder access.",
                    "Module launch cancelled", "Completed textures have been saved. Processing can resume next time.", "Base game"
            },
            // es — Español
            {
                    "Preparando texturas del módulo", "Convirtiendo", "Verificando resultados", "Comprobando texturas", "Se detendrá tras el archivo actual…",
                    "Tiempo total: estimación tras comprobar las texturas", "Tiempo total: calculando la velocidad de procesamiento…",
                    "Proceso completo: ≈ %1$s · restante ≈ %2$s",
                    "ETC1 (RGBA para texturas transparentes)", "Texturas por convertir: ", "Formato del dispositivo: ", "Ya preparadas: ",
                    "Se reemplazarán los archivos de textura del módulo. Para recuperar los originales se necesita el archivo original del módulo.",
                    "El proceso puede tardar varios minutos. Si se cancela, los archivos terminados se conservan y se puede continuar después. El módulo se iniciará automáticamente tras la verificación.",
                    "¿Preparar texturas del módulo?", "Convertir",
                    "No se pudieron preparar las texturas", "El módulo no se ha iniciado. Los archivos terminados se conservan; se comprobarán de nuevo en el próximo inicio.", "Cerrar",
                    "Los archivos del módulo cambiaron durante el proceso. Vuelve a comprobarlos.",
                    "Una textura está dañada o usa un formato no compatible. Recupérala del archivo original del módulo.",
                    "El módulo contiene un enlace o una ruta fuera de su carpeta. Instala una copia normal de los archivos del módulo.",
                    "El tamaño o la estructura de carpetas del módulo supera el límite admitido.",
                    "No se pudieron leer o escribir los archivos del módulo. Comprueba el acceso a la carpeta y el espacio libre.",
                    "El conversor de texturas no está disponible en esta versión.",
                    "No se pudo comprobar la compatibilidad de formatos de la GPU. Vuelve a la aplicación e inténtalo de nuevo.",
                    "El controlador gráfico no confirmó la compatibilidad con ASTC ni ETC1.",
                    "No se pudo procesar una textura. Comprueba el espacio libre y los archivos del módulo.",
                    "No se pudo abrir el módulo", "Comprueba los archivos del módulo y el acceso a la carpeta.",
                    "Inicio del módulo cancelado", "Las texturas terminadas se han guardado. Podrás continuar la próxima vez.", "Juego base"
            },
            // fr — Français
            {
                    "Préparation des textures du module", "Conversion", "Vérification des résultats", "Vérification des textures", "Arrêt après le fichier en cours…",
                    "Durée totale : estimation après l'analyse des textures", "Durée totale : mesure de la vitesse de traitement…",
                    "Traitement complet : ≈ %1$s · restant ≈ %2$s",
                    "ETC1 (RGBA pour les textures transparentes)", "Textures à convertir : ", "Format de l'appareil : ", "Déjà prêtes : ",
                    "Les fichiers de texture du module seront remplacés. La restauration des originaux nécessite l'archive d'origine du module.",
                    "Le traitement peut prendre plusieurs minutes. En cas d'annulation, les fichiers terminés sont conservés et le traitement pourra reprendre. Le module démarrera automatiquement après vérification.",
                    "Préparer les textures du module ?", "Convertir",
                    "Échec de la préparation des textures", "Le module n'a pas démarré. Les fichiers terminés sont conservés et seront revérifiés au prochain lancement.", "Fermer",
                    "Les fichiers du module ont changé pendant le traitement. Relancez la vérification.",
                    "Une texture est endommagée ou utilise un format non pris en charge. Restaurez-la depuis l'archive d'origine du module.",
                    "Le module contient un lien ou un chemin hors de son dossier. Installez une copie normale des fichiers du module.",
                    "La taille ou l'arborescence du module dépasse la limite prise en charge.",
                    "Impossible de lire ou d'écrire les fichiers du module. Vérifiez l'accès au dossier et l'espace libre.",
                    "Le convertisseur de textures n'est pas disponible dans cette version.",
                    "Impossible de vérifier les formats pris en charge par le GPU. Revenez dans l'application et réessayez.",
                    "Le pilote graphique n'a pas confirmé la prise en charge d'ASTC ou d'ETC1.",
                    "Impossible de traiter une texture. Vérifiez l'espace libre et les fichiers du module.",
                    "Impossible d'ouvrir le module", "Vérifiez les fichiers du module et l'accès au dossier.",
                    "Lancement du module annulé", "Les textures terminées ont été enregistrées. Le traitement pourra reprendre au prochain lancement.", "Jeu de base"
            },
            // hu — Magyar
            {
                    "A modul textúráinak előkészítése", "Konvertálás", "Eredmény ellenőrzése", "Textúrák ellenőrzése", "Leállítás az aktuális fájl után…",
                    "Teljes idő: becslés a textúrák ellenőrzése után", "Teljes idő: feldolgozási sebesség mérése…",
                    "Teljes feldolgozás: ≈ %1$s · hátralévő idő ≈ %2$s",
                    "ETC1 (RGBA az átlátszó textúrákhoz)", "Konvertálandó textúrák: ", "Eszközformátum: ", "Már kész: ",
                    "A modul textúrafájljai lecserélődnek. Az eredeti fájlok visszaállításához szükség lesz a modul eredeti archívumára.",
                    "A feldolgozás több percig is eltarthat. Megszakításkor az elkészült fájlok megmaradnak, és később folytatható. Az ellenőrzés után a modul automatikusan elindul.",
                    "Előkészíti a modul textúráit?", "Konvertálás",
                    "A textúrák előkészítése sikertelen", "A modul nem indult el. Az elkészült fájlok megmaradnak; a következő indításkor ismét ellenőrizzük őket.", "Bezárás",
                    "A modul fájljai feldolgozás közben megváltoztak. Indítsa újra az ellenőrzést.",
                    "Egy textúra sérült vagy nem támogatott formátumú. Állítsa vissza a modul eredeti archívumából.",
                    "A modul hivatkozást vagy a mappáján kívülre mutató útvonalat tartalmaz. Telepítse a modulfájlok szokásos másolatát.",
                    "A modul mérete vagy mappaszerkezete meghaladja a támogatott korlátot.",
                    "A modulfájlok nem olvashatók vagy írhatók. Ellenőrizze a mappa hozzáférését és a szabad helyet.",
                    "Ebben a verzióban nem érhető el a textúrakonvertáló.",
                    "Nem ellenőrizhető a GPU formátumtámogatása. Térjen vissza az alkalmazásba, és próbálja újra.",
                    "A grafikus illesztőprogram nem erősítette meg az ASTC vagy ETC1 támogatását.",
                    "Egy textúra nem dolgozható fel. Ellenőrizze a szabad helyet és a modulfájlokat.",
                    "A modul nem nyitható meg", "Ellenőrizze a modulfájlokat és a mappa hozzáférését.",
                    "A modul indítása megszakítva", "Az elkészült textúrákat mentettük. A következő indításkor folytatható.", "Alapjáték"
            },
            // ja — 日本語
            {
                    "モジュールのテクスチャを準備", "変換中", "結果を検証中", "テクスチャを確認中", "現在のファイルが終わり次第停止します…",
                    "合計時間：テクスチャ確認後に推定", "合計時間：処理速度を測定中…",
                    "全処理時間：約 %1$s · 残り約 %2$s",
                    "ETC1（透明テクスチャには RGBA を使用）", "変換が必要なテクスチャ：", "端末の形式：", "処理済み：",
                    "モジュールのテクスチャファイルは置き換えられます。元に戻すには、元のモジュールのアーカイブが必要です。",
                    "処理には数分かかる場合があります。キャンセルしても完了したファイルは残り、次回続行できます。検証後、モジュールは自動的に起動します。",
                    "モジュールのテクスチャを準備しますか？", "変換",
                    "テクスチャの準備に失敗しました", "モジュールは起動していません。完了したファイルは保持され、次回の起動時に再確認されます。", "閉じる",
                    "処理中にモジュールのファイルが変更されました。もう一度確認してください。",
                    "テクスチャが破損しているか、形式がサポートされていません。元のモジュールのアーカイブから復元してください。",
                    "モジュールにリンクまたはフォルダー外を指すパスがあります。通常のファイルのコピーをインストールしてください。",
                    "モジュールのサイズまたはフォルダー構造が対応範囲を超えています。",
                    "モジュールのファイルを読み書きできません。フォルダーのアクセス権と空き容量を確認してください。",
                    "このビルドではテクスチャ変換を利用できません。",
                    "GPU の対応形式を確認できません。アプリに戻って再度起動してください。",
                    "グラフィックドライバーが ASTC または ETC1 への対応を確認できませんでした。",
                    "テクスチャを処理できません。空き容量とモジュールのファイルを確認してください。",
                    "モジュールを開けません", "モジュールのファイルとフォルダーのアクセス権を確認してください。",
                    "モジュールの起動をキャンセルしました", "完了したテクスチャは保存されました。次回の起動時に続行できます。", "基本ゲーム"
            },
            // pl — Polski
            {
                    "Przygotowywanie tekstur modułu", "Konwertowanie", "Sprawdzanie wyników", "Sprawdzanie tekstur", "Zatrzymywanie po bieżącym pliku…",
                    "Całkowity czas: oszacowanie po sprawdzeniu tekstur", "Całkowity czas: pomiar szybkości przetwarzania…",
                    "Pełne przetwarzanie: ≈ %1$s · pozostało ≈ %2$s",
                    "ETC1 (RGBA dla przezroczystych tekstur)", "Tekstury do konwersji: ", "Format urządzenia: ", "Już gotowe: ",
                    "Pliki tekstur modułu zostaną zastąpione. Do przywrócenia oryginałów potrzebne będzie oryginalne archiwum modułu.",
                    "Przetwarzanie może potrwać kilka minut. Po anulowaniu gotowe pliki zostaną zachowane i będzie można kontynuować później. Po sprawdzeniu moduł uruchomi się automatycznie.",
                    "Przygotować tekstury modułu?", "Konwertuj",
                    "Nie udało się przygotować tekstur", "Moduł nie został uruchomiony. Gotowe pliki zostaną zachowane i sprawdzone ponownie przy następnym uruchomieniu.", "Zamknij",
                    "Pliki modułu zmieniły się podczas przetwarzania. Uruchom sprawdzanie ponownie.",
                    "Tekstura jest uszkodzona lub ma nieobsługiwany format. Przywróć ją z oryginalnego archiwum modułu.",
                    "Moduł zawiera dowiązanie lub ścieżkę poza swoim folderem. Zainstaluj zwykłą kopię plików modułu.",
                    "Rozmiar lub struktura folderów modułu przekracza obsługiwany limit.",
                    "Nie można odczytać lub zapisać plików modułu. Sprawdź dostęp do folderu i wolne miejsce.",
                    "Konwerter tekstur jest niedostępny w tej wersji.",
                    "Nie można sprawdzić obsługiwanych formatów GPU. Wróć do aplikacji i spróbuj ponownie.",
                    "Sterownik graficzny nie potwierdził obsługi ASTC ani ETC1.",
                    "Nie można przetworzyć tekstury. Sprawdź wolne miejsce i pliki modułu.",
                    "Nie można otworzyć modułu", "Sprawdź pliki modułu i dostęp do folderu.",
                    "Uruchamianie modułu anulowane", "Gotowe tekstury zostały zapisane. Można kontynuować przy następnym uruchomieniu.", "Gra podstawowa"
            },
            // ru — Русский
            {
                    "Подготовка текстур мода", "Конвертация", "Проверка результата", "Проверка текстур", "Остановка после текущего файла…",
                    "Общее время: расчёт после проверки текстур", "Общее время: расчёт по скорости обработки…",
                    "Полная обработка: ≈ %1$s · осталось ≈ %2$s",
                    "ETC1 (RGBA для прозрачных текстур)", "Требуют конвертации: ", "Формат устройства: ", "Уже готовы: ",
                    "Файлы текстур мода будут заменены. Чтобы вернуть оригиналы, потребуется исходный архив мода.",
                    "Обработка может занять несколько минут. После отмены готовые файлы сохранятся, и обработку можно продолжить. После проверки мод запустится автоматически.",
                    "Подготовить текстуры мода?", "Конвертировать",
                    "Не удалось подготовить текстуры", "Мод не запущен. Уже готовые файлы сохраняются; повторный запуск продолжит проверку.", "Закрыть",
                    "Файлы мода изменились во время обработки. Запустите проверку ещё раз.",
                    "Текстура повреждена или имеет неподдерживаемый формат. Восстановите её из исходного архива мода.",
                    "В папке мода обнаружена ссылка или путь за пределы папки. Установите обычную копию файлов мода.",
                    "Размер или структура папки мода превышает допустимый предел.",
                    "Не удалось прочитать или записать файлы мода. Проверьте доступ к папке и свободное место.",
                    "В этой сборке недоступен конвертер текстур.",
                    "Не удалось проверить поддержку форматов на GPU. Вернитесь в приложение и повторите запуск.",
                    "Графический драйвер не подтвердил поддержку ASTC или ETC1.",
                    "Не удалось обработать текстуру. Проверьте свободное место и файлы мода.",
                    "Не удалось открыть мод", "Проверьте файлы мода и доступ к папке.",
                    "Запуск мода отменён", "Готовые текстуры сохранены. При следующем запуске обработка продолжится.", "Основная игра"
            },
            // tr — Türkçe
            {
                    "Modül dokuları hazırlanıyor", "Dönüştürülüyor", "Sonuçlar doğrulanıyor", "Dokular kontrol ediliyor", "Geçerli dosyadan sonra durduruluyor…",
                    "Toplam süre: doku taramasından sonra tahmin edilecek", "Toplam süre: işlem hızı ölçülüyor…",
                    "Tüm işlem: ≈ %1$s · kalan ≈ %2$s",
                    "ETC1 (saydam dokular için RGBA)", "Dönüştürülecek dokular: ", "Cihaz biçimi: ", "Hazır olanlar: ",
                    "Modülün doku dosyaları değiştirilecek. Orijinalleri geri yüklemek için modülün özgün arşivi gerekir.",
                    "İşlem birkaç dakika sürebilir. İptal edilirse tamamlanan dosyalar korunur ve daha sonra devam edilebilir. Doğrulamadan sonra modül otomatik olarak açılır.",
                    "Modül dokuları hazırlansın mı?", "Dönüştür",
                    "Doku hazırlığı başarısız", "Modül başlatılmadı. Tamamlanan dosyalar korunur ve sonraki açılışta yeniden kontrol edilir.", "Kapat",
                    "Modül dosyaları işlem sırasında değişti. Kontrolü yeniden başlatın.",
                    "Bir doku bozuk veya desteklenmeyen biçimde. Modülün özgün arşivinden geri yükleyin.",
                    "Modülde bir bağlantı veya klasörünün dışına çıkan yol var. Modül dosyalarının normal bir kopyasını yükleyin.",
                    "Modülün boyutu veya klasör yapısı desteklenen sınırı aşıyor.",
                    "Modül dosyaları okunamadı veya yazılamadı. Klasör erişimini ve boş alanı kontrol edin.",
                    "Bu sürümde doku dönüştürücü kullanılamıyor.",
                    "GPU biçim desteği kontrol edilemedi. Uygulamaya dönüp yeniden başlatın.",
                    "Grafik sürücüsü ASTC veya ETC1 desteğini doğrulamadı.",
                    "Bir doku işlenemedi. Boş alanı ve modül dosyalarını kontrol edin.",
                    "Modül açılamadı", "Modül dosyalarını ve klasör erişimini kontrol edin.",
                    "Modül başlatma iptal edildi", "Tamamlanan dokular kaydedildi. Sonraki açılışta devam edilebilir.", "Ana oyun"
            }
    };

    static {
        if (!Arrays.equals(CODES, MBTouchStrings.codes()) || CODES.length != STRINGS.length) {
            throw new AssertionError("module language list differs from game languages");
        }
        for (int language = 0; language < CODES.length; ++language) {
            if (STRINGS[language].length != Text.values().length) {
                throw new AssertionError("incomplete module strings for " + CODES[language]);
            }
            for (String value : STRINGS[language]) {
                if (value == null || value.trim().length() == 0) {
                    throw new AssertionError("empty module string for " + CODES[language]);
                }
            }
        }
    }

    private MBModTextureStrings() {}

    public static String get(String language, Text text) {
        if (text == null) throw new IllegalArgumentException("text must not be null");
        return STRINGS[MBTouchStrings.indexOf(language)][text.ordinal()];
    }

    public static String estimate(String language, long totalMs, long remainingMs) {
        return String.format(Locale.ROOT, get(language, Text.TIME_ESTIMATE),
                duration(totalMs), duration(remainingMs));
    }

    static String duration(long milliseconds) {
        long seconds = Math.max(0L, (milliseconds + 999L) / 1000L);
        long hours = seconds / 3600L;
        long minutes = (seconds / 60L) % 60L;
        long remainder = seconds % 60L;
        if (hours > 0L) {
            return String.format(Locale.ROOT, "%d:%02d:%02d", hours, minutes, remainder);
        }
        return String.format(Locale.ROOT, "%d:%02d", seconds / 60L, remainder);
    }
}
