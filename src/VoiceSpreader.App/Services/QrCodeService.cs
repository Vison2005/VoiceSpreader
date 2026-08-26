using Microsoft.UI.Xaml.Media.Imaging;
using QRCoder;
using Windows.Storage.Streams;

namespace VoiceSpreader.App.Services;

public static class QrCodeService
{
    public static async Task<BitmapImage> CreateImageAsync(string payload)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(payload);
        using var data = QRCodeGenerator.GenerateQrCode(payload, QRCodeGenerator.ECCLevel.M);
        using var code = new PngByteQRCode(data);
        var png = code.GetGraphic(
            6,
            [0x18, 0x21, 0x2A, 0xFF],
            [0xFF, 0xFF, 0xFF, 0xFF],
            drawQuietZones: true);

        using var stream = new InMemoryRandomAccessStream();
        using (var writer = new DataWriter(stream.GetOutputStreamAt(0)))
        {
            writer.WriteBytes(png);
            await writer.StoreAsync();
        }
        stream.Seek(0);
        var image = new BitmapImage();
        await image.SetSourceAsync(stream);
        return image;
    }
}
