// Renders the handbook: SVG diagrams -> PNG, and HTML -> PDF, with headless Chromium (Playwright).
//   node render.js png <in.svg> <out.png>
//   node render.js pdf <in.html> <out.pdf> <footer text>
const { chromium } = require('playwright');
const path = require('path');
const fs = require('fs');

(async () => {
  const [mode, input, output, footer] = process.argv.slice(2);
  const browser = await chromium.launch();
  try {
    if (mode === 'png') {
      const svg = fs.readFileSync(input, 'utf8');
      const m = svg.match(/viewBox="0 0 (\d+) (\d+)"/);
      const page = await browser.newPage({
        viewport: { width: Number(m[1]), height: Number(m[2]) },
        deviceScaleFactor: 3,
      });
      await page.setContent(`<body style="margin:0">${svg}</body>`);
      await page.evaluate(() => document.fonts.ready);
      await page.screenshot({ path: output });
    } else {
      const page = await browser.newPage();
      await page.goto('file://' + path.resolve(input));
      await page.evaluate(() => document.fonts.ready);
      await page.pdf({
        path: output,
        format: 'A4',
        printBackground: true,
        displayHeaderFooter: true,
        headerTemplate: '<span></span>',
        footerTemplate:
          `<div style="width:100%;font-size:7px;color:#777;padding:0 12mm;display:flex;` +
          `justify-content:space-between;font-family:'WenQuanYi Zen Hei','Liberation Sans',sans-serif">` +
          `<span>${footer || ''}</span><span><span class="pageNumber"></span> / <span class="totalPages"></span></span></div>`,
        margin: { top: '11mm', bottom: '13mm', left: '12mm', right: '12mm' },
      });
    }
  } finally {
    await browser.close();
  }
})().catch((e) => {
  console.error(e);
  process.exit(1);
});
