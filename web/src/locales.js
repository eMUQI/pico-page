export const defaultLocale = "zh-Hans";
export const languagePreferenceKey = "picopage.language";
export const locales = {
  "zh-Hans": { prefix: "", name: "简体中文" },
  "zh-Hant": { prefix: "zh-hant", name: "繁體中文" },
  en: { prefix: "en", name: "English" },
};
export const translatedLocales = Object.entries(locales).filter(([locale]) => locale !== defaultLocale);

const pagePaths = new Set(["/", "/docs/", "/install/", "/changelog/"]);

export function localizeHref(locale, href) {
  const [path] = href.split(/[?#]/, 1);
  const { prefix } = locales[locale];
  return prefix && pagePaths.has(path) ? `/${prefix}${href}` : href;
}

export function translator(locale, catalog) {
  return (message) => {
    if (locale === defaultLocale) return message;
    const translation = catalog[locale]?.[message];
    if (typeof translation !== "string") throw new Error(`Missing ${locale} translation for ${message}`);
    return translation;
  };
}
