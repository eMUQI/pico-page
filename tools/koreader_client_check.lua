-- Host-only instrumentation loaded through KOReader's documented user-patch entry point.
-- The reader, document engine, KOSync plugin, HTTP client and event handlers are unmodified.
local UIManager = require("ui/uimanager")
local ReaderUI = require("apps/reader/readerui")
local json = require("json")
local home = assert(os.getenv("KO_HOME"))
local function read(name)
    local file = io.open(home .. "/" .. name, "rb")
    if not file then return nil end
    local content = file:read("*a")
    file:close()
    return json.decode(content)
end
local fixture = assert(read("fixture.json"))
local results = {}
local function finish(ok, detail)
    results.ok, results.detail = ok, detail
    local file = assert(io.open(home .. "/result.json", "wb"))
    file:write(json.encode(results))
    file:close()
    if ok and ReaderUI.instance then ReaderUI.instance:onClose() end
    UIManager:quit()
end
local function guarded(callback)
    return function()
        local ok, err = xpcall(callback, debug.traceback)
        if not ok then finish(false, err) end
    end
end
local function after(delay, callback)
    UIManager:scheduleIn(delay, guarded(callback))
end

after(3, function()
    local reader = assert(ReaderUI.instance, "Reader did not initialize")
    assert(reader.kosync and reader.rolling, "Real KOSync plugin or rolling reader unavailable")
    results.version = require("version"):getCurrentRevision()
    results.dom = reader.doc_settings:readSetting("cre_dom_version")
    assert(results.dom == 20260812, "Unexpected client DOM profile")
    if fixture.resume then
        assert(reader.rolling:getLastProgress() == fixture.pointer, "Official client did not restore the saved sync position")
        assert(reader.document:getTextFromXPointers(fixture.pointer, fixture.end_pointer) == fixture.text,
            "Reopened official client shows a different passage")
        results.restored = reader.rolling:getLastProgress()
        results.font_size = reader.document.configurable.font_size
        return finish(true, "Official client restored its saved sync position after restart")
    end
    assert(reader.document.configurable.font_size == fixture.font_size, "Initial official font setting was not applied")
    after(1, function()
        local plugin = reader.kosync
        local client = require("KOSyncClient"):new{
            custom_url = fixture.endpoint, service_spec = plugin.path .. "/api.json",
        }
        assert(client:register(fixture.username, fixture.userkey), "Disposable account registration failed")
        assert(client:authorize(fixture.username, fixture.userkey), "Disposable account authorization failed")
        assert(not client:authorize(fixture.username, "invalid-fixture-key"), "Invalid credentials accepted")
        client:get_progress(fixture.username, fixture.userkey, fixture.document, function(ok, body)
            assert(ok and not body.percentage, "Expected initially absent progress")
            results.empty_record = true
        end)
        plugin.settings = {
            custom_server = fixture.endpoint, username = fixture.username, userkey = fixture.userkey,
            checksum_method = 0, auto_sync = false, send_metadata = false,
        }
        plugin.device_id = "official-koreader-validation"
        assert(plugin:getDocumentDigest() == fixture.document, "Original EPUB identity mismatch")
        reader.rolling:onGotoPage(math.max(3, math.floor(reader.document.info.number_of_pages / 3)))
        results.uploaded = plugin:getLastProgress()
        results.font_size = reader.document.configurable.font_size
        assert(results.font_size == fixture.font_size, "Official font setting was not applied")
        results.upload_page = reader.rolling.current_page
        plugin:updateProgress(false, true)

        local attempts = 0
        local function receive()
            attempts = attempts + 1
            local ready = read("native-ready.json")
            if not ready then
                assert(attempts < 150, "Native peer did not publish progress")
                after(.2, receive)
                return
            end
            assert(reader.document:getTextFromXPointers(ready.pointer, ready.end_pointer) == ready.text,
                "Native peer recovered different text from the official client upload")
            assert(reader.document:getPageFromXPointer(results.uploaded) == results.upload_page,
                "Official uploaded position does not belong to the displayed page")
            results.upload_passage = ready.text
            plugin:getProgress(false, true)
            after(1, function()
                assert(plugin:getLastProgress() == fixture.pointer, "Official plugin did not apply native XPointer")
                local actual = reader.document:getTextFromXPointers(fixture.pointer, fixture.end_pointer)
                assert(actual == fixture.text, "Official reader resumed a different passage")
                assert(reader.document:getPageFromXPointer(fixture.pointer) == reader.rolling.current_page,
                    "Official reader did not display the target page")
                results.downloaded = plugin:getLastProgress()
                results.download_page = reader.rolling.current_page
                results.passage = actual
                results.empty_record = results.empty_record == true
                after(3, function()
                    require("device").screen.bb:writePNG(home .. "/client-page.png")
                    finish(true, "Official reader push, native recovery, native upload and official reader pull passed")
                end)
            end)
        end
        after(.2, receive)
    end)
end)
