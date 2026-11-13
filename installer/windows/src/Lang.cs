// Lang.cs: Spanish when Windows' display language is Spanish, English otherwise.
using System.Globalization;

namespace Sapphire
{
	static class L
	{
		/* SAPPHIRE_LANG=es|en overrides Windows' language (testing) */
		public static readonly bool Es = System.Environment.GetEnvironmentVariable("SAPPHIRE_LANG") != null ?
		    System.Environment.GetEnvironmentVariable("SAPPHIRE_LANG") == "es" :
		    CultureInfo.CurrentUICulture.TwoLetterISOLanguageName == "es";

		public static string T(string es, string en)
		{
			return Es ? es : en;
		}

		/* the phone's side (sapphire.sh): its messages and screen in the same language */
		public static string Code { get { return Es ? "es" : "en"; } }
	}
}
