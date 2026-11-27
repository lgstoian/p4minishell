// SPDX-FileCopyrightText: 2026 Stoian Alexandru
// SPDX-License-Identifier: MIT
// @module p4sync_gui/tests/mock-data
// @layer leaf
// @domain P4Sync desktop client
// @provides offline JSON-lines fixtures for protocol tests
// @status prototype
// @test Program.cs
namespace P4SyncBridge.Tests;

/// <summary>Representative bridge frames - no hardware, no network, no bridge.py needed.</summary>
public static class MockBridgeData
{
    public const string Devices =
        """{"id":1,"ok":true,"data":{"devices":[{"port":"COM6","board":"tab5","proto":"p4sync-1","status":"ready"},{"port":"/dev/ttyUSB0","board":"c6","proto":"p4sync-1","status":"busy"}]}}""";

    // Shape emitted by bridge.py --mock: screenshot limit is now advertised.
    public const string Status =
        """{"id":2,"ok":true,"data":{"port":"MOCK1","board":"jc1060p470c","proto":"p4sync-1","sd":"mounted","lock":"unlocked","caps":{"proto":"p4sync-1","board":"jc1060p470c","sd":"mounted","lock":"unlocked","magic.sdfx":"SDFX","magic.pimx":"PIMX","magic.bmpx":"BMPX","limit.transfer":"1048576","limit.screenshot":"1048576","limit.rx":"2097152","sync.shot_magic":"BMPX"},"display":[1024,600]}}""";

    // Old firmware does not advertise limit.screenshot; it stays optional.
    public const string StatusWithoutShotLimit =
        """{"id":13,"ok":true,"data":{"port":"COM6","board":"jc1060p470c","proto":"p4sync-1","sd":"mounted","lock":"unlocked","caps":{"proto":"p4sync-1","board":"jc1060p470c","sd":"mounted","lock":"unlocked","magic.sdfx":"SDFX","magic.pimx":"PIMX","magic.bmpx":"BMPX","limit.transfer":"1048576"}}}""";

    public const string StatusBadShotLimit =
        """{"id":14,"ok":true,"data":{"port":"COM6","board":"jc1060p470c","proto":"p4sync-1","sd":"mounted","lock":"unlocked","caps":{"proto":"p4sync-1","board":"jc1060p470c","sd":"mounted","lock":"unlocked","magic.sdfx":"SDFX","magic.pimx":"PIMX","magic.bmpx":"BMPX","limit.transfer":"1048576","limit.screenshot":"huge"}}}""";

    public const string StatusMismatch =
        """{"id":3,"ok":true,"data":{"port":"COM3","board":"jc1060p470c","proto":"p4sync-0","sd":"absent","lock":"locked","caps":{}}}""";

    public const string Plan =
        """{"id":4,"ok":true,"data":{"actions":[{"op":"push","path":"notes.md","reason":"newer local mtime","size":42},{"op":"skip","path":"todo.md","reason":"unchanged"}],"summary":{"files":2,"push":1,"skip":1}}}""";

    public const string Progress =
        """{"event":"progress","id":5,"pct":45,"message":"receive notes.md (45%)"}""";

    public const string Log =
        """{"event":"log","id":5,"level":"warn","message":"sd descriptor pool low"}""";

    public const string Screenshot =
        """{"id":6,"ok":true,"data":{"out":"C:\\temp\\shot.png","path":"C:\\temp\\shot.png","width":320,"height":240}}""";

    public const string ScreenshotBase64 =
        """{"id":8,"ok":true,"data":{"out":"shot.png","base64":"iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+ip1sAAAAASUVORK5CYIIA"}}""";

    // Contract after the bridge sibling's change: path + png_base64 + integrity.
    public const string ScreenshotFull =
        """{"id":15,"ok":true,"data":{"port":"MOCK1","out":"C:\\temp\\shot.png","path":"C:\\temp\\shot.png","png":"C:\\temp\\shot.png","png_base64":"iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+ip1sAAAAASUVORK5CYIIA","sha256":"2edda04ac06bd5fb14f3651024824c34a7a15e5f17dd0d3928029ca1a6a0c962","size":69,"bytes":69,"width":1,"height":1}}""";

    public const string ScreenshotBadBase64 =
        """{"id":16,"ok":true,"data":{"port":"MOCK1","png_base64":"not!base64","out":"C:\\temp\\shot.png"}}""";

    public const string Verify =
        """{"id":9,"ok":true,"data":{"remote":"notes.md","ok":true,"crc32":3156489900}}""";

    public const string Shell =
        """{"id":12,"ok":true,"data":{"port":"COM6","command":"ver","output":"P4MiniShell 1.6.0\r\nbuild ok","errorlevel":0,"ok":true}}""";

    public const string Pkg =
        """{"id":10,"ok":true,"data":{"verb":"list","package":"","command":"pkg list /b","output":"NOTES APPINFO","rows":["NOTES","WEB"],"ok":true}}""";

    public const string FullImage =
        """{"id":11,"ok":true,"data":{"manifest_path":"C:\\temp\\image.json","manifest":{"version":1,"files":2},"failures":[]}}""";

    public const string FullImageStale =
        """{"id":17,"ok":true,"data":{"port":"MOCK1","manifest_path":"C:\\temp\\image.json","manifest":{"version":1,"files":2},"failures":0,"ok":true,"stale_manifest":true,"rebuilt":true,"stale_reason":"manifest shot is not a path"}}""";

    public const string FullImageRejected =
        """{"id":18,"ok":true,"data":{"port":"MOCK1","manifest_path":"C:\\temp\\image.json","failures":1,"ok":false,"stale_manifest":true,"rebuilt":true,"stale_reason":"invalid image.json: shot must be a path"}}""";

    public const string Error =
        """{"id":7,"ok":false,"error":"proto mismatch: device='p4sync-0' host='p4sync-1'"}""";

    public const string UnknownFrame = """{"hello":"bridge"}""";
}
