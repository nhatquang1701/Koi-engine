#include "koi/game_state.hpp"

#include <algorithm>
#include <atomic>
#include <array>
#include <bit>
#include <charconv>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <utility>

#include "koi/detail/compatibility_mirror.hpp"
#include "koi/detail/feature_state.hpp"
#include "koi/position.hpp"

namespace koi {

namespace {

constexpr std::array<std::uint64_t, 781> kPolyglotRandom{
+0x9D39247E33776D41ULL,
    0x2AF7398005AAA5C7ULL,
    0x44DB015024623547ULL,
    0x9C15F73E62A76AE2ULL,
    0x75834465489C0C89ULL,
    0x3290AC3A203001BFULL,
    0x0FBBAD1F61042279ULL,
    0xE83A908FF2FB60CAULL,
    0x0D7E765D58755C10ULL,
    0x1A083822CEAFE02DULL,
    0x9605D5F0E25EC3B0ULL,
    0xD021FF5CD13A2ED5ULL,
    0x40BDF15D4A672E32ULL,
    0x011355146FD56395ULL,
    0x5DB4832046F3D9E5ULL,
    0x239F8B2D7FF719CCULL,
    0x05D1A1AE85B49AA1ULL,
    0x679F848F6E8FC971ULL,
    0x7449BBFF801FED0BULL,
    0x7D11CDB1C3B7ADF0ULL,
    0x82C7709E781EB7CCULL,
    0xF3218F1C9510786CULL,
    0x331478F3AF51BBE6ULL,
    0x4BB38DE5E7219443ULL,
    0xAA649C6EBCFD50FCULL,
    0x8DBD98A352AFD40BULL,
    0x87D2074B81D79217ULL,
    0x19F3C751D3E92AE1ULL,
    0xB4AB30F062B19ABFULL,
    0x7B0500AC42047AC4ULL,
    0xC9452CA81A09D85DULL,
    0x24AA6C514DA27500ULL,
    0x4C9F34427501B447ULL,
    0x14A68FD73C910841ULL,
    0xA71B9B83461CBD93ULL,
    0x03488B95B0F1850FULL,
    0x637B2B34FF93C040ULL,
    0x09D1BC9A3DD90A94ULL,
    0x3575668334A1DD3BULL,
    0x735E2B97A4C45A23ULL,
    0x18727070F1BD400BULL,
    0x1FCBACD259BF02E7ULL,
    0xD310A7C2CE9B6555ULL,
    0xBF983FE0FE5D8244ULL,
    0x9F74D14F7454A824ULL,
    0x51EBDC4AB9BA3035ULL,
    0x5C82C505DB9AB0FAULL,
    0xFCF7FE8A3430B241ULL,
    0x3253A729B9BA3DDEULL,
    0x8C74C368081B3075ULL,
    0xB9BC6C87167C33E7ULL,
    0x7EF48F2B83024E20ULL,
    0x11D505D4C351BD7FULL,
    0x6568FCA92C76A243ULL,
    0x4DE0B0F40F32A7B8ULL,
    0x96D693460CC37E5DULL,
    0x42E240CB63689F2FULL,
    0x6D2BDCDAE2919661ULL,
    0x42880B0236E4D951ULL,
    0x5F0F4A5898171BB6ULL,
    0x39F890F579F92F88ULL,
    0x93C5B5F47356388BULL,
    0x63DC359D8D231B78ULL,
    0xEC16CA8AEA98AD76ULL,
    0x5355F900C2A82DC7ULL,
    0x07FB9F855A997142ULL,
    0x5093417AA8A7ED5EULL,
    0x7BCBC38DA25A7F3CULL,
    0x19FC8A768CF4B6D4ULL,
    0x637A7780DECFC0D9ULL,
    0x8249A47AEE0E41F7ULL,
    0x79AD695501E7D1E8ULL,
    0x14ACBAF4777D5776ULL,
    0xF145B6BECCDEA195ULL,
    0xDABF2AC8201752FCULL,
    0x24C3C94DF9C8D3F6ULL,
    0xBB6E2924F03912EAULL,
    0x0CE26C0B95C980D9ULL,
    0xA49CD132BFBF7CC4ULL,
    0xE99D662AF4243939ULL,
    0x27E6AD7891165C3FULL,
    0x8535F040B9744FF1ULL,
    0x54B3F4FA5F40D873ULL,
    0x72B12C32127FED2BULL,
    0xEE954D3C7B411F47ULL,
    0x9A85AC909A24EAA1ULL,
    0x70AC4CD9F04F21F5ULL,
    0xF9B89D3E99A075C2ULL,
    0x87B3E2B2B5C907B1ULL,
    0xA366E5B8C54F48B8ULL,
    0xAE4A9346CC3F7CF2ULL,
    0x1920C04D47267BBDULL,
    0x87BF02C6B49E2AE9ULL,
    0x092237AC237F3859ULL,
    0xFF07F64EF8ED14D0ULL,
    0x8DE8DCA9F03CC54EULL,
    0x9C1633264DB49C89ULL,
    0xB3F22C3D0B0B38EDULL,
    0x390E5FB44D01144BULL,
    0x5BFEA5B4712768E9ULL,
    0x1E1032911FA78984ULL,
    0x9A74ACB964E78CB3ULL,
    0x4F80F7A035DAFB04ULL,
    0x6304D09A0B3738C4ULL,
    0x2171E64683023A08ULL,
    0x5B9B63EB9CEFF80CULL,
    0x506AACF489889342ULL,
    0x1881AFC9A3A701D6ULL,
    0x6503080440750644ULL,
    0xDFD395339CDBF4A7ULL,
    0xEF927DBCF00C20F2ULL,
    0x7B32F7D1E03680ECULL,
    0xB9FD7620E7316243ULL,
    0x05A7E8A57DB91B77ULL,
    0xB5889C6E15630A75ULL,
    0x4A750A09CE9573F7ULL,
    0xCF464CEC899A2F8AULL,
    0xF538639CE705B824ULL,
    0x3C79A0FF5580EF7FULL,
    0xEDE6C87F8477609DULL,
    0x799E81F05BC93F31ULL,
    0x86536B8CF3428A8CULL,
    0x97D7374C60087B73ULL,
    0xA246637CFF328532ULL,
    0x043FCAE60CC0EBA0ULL,
    0x920E449535DD359EULL,
    0x70EB093B15B290CCULL,
    0x73A1921916591CBDULL,
    0x56436C9FE1A1AA8DULL,
    0xEFAC4B70633B8F81ULL,
    0xBB215798D45DF7AFULL,
    0x45F20042F24F1768ULL,
    0x930F80F4E8EB7462ULL,
    0xFF6712FFCFD75EA1ULL,
    0xAE623FD67468AA70ULL,
    0xDD2C5BC84BC8D8FCULL,
    0x7EED120D54CF2DD9ULL,
    0x22FE545401165F1CULL,
    0xC91800E98FB99929ULL,
    0x808BD68E6AC10365ULL,
    0xDEC468145B7605F6ULL,
    0x1BEDE3A3AEF53302ULL,
    0x43539603D6C55602ULL,
    0xAA969B5C691CCB7AULL,
    0xA87832D392EFEE56ULL,
    0x65942C7B3C7E11AEULL,
    0xDED2D633CAD004F6ULL,
    0x21F08570F420E565ULL,
    0xB415938D7DA94E3CULL,
    0x91B859E59ECB6350ULL,
    0x10CFF333E0ED804AULL,
    0x28AED140BE0BB7DDULL,
    0xC5CC1D89724FA456ULL,
    0x5648F680F11A2741ULL,
    0x2D255069F0B7DAB3ULL,
    0x9BC5A38EF729ABD4ULL,
    0xEF2F054308F6A2BCULL,
    0xAF2042F5CC5C2858ULL,
    0x480412BAB7F5BE2AULL,
    0xAEF3AF4A563DFE43ULL,
    0x19AFE59AE451497FULL,
    0x52593803DFF1E840ULL,
    0xF4F076E65F2CE6F0ULL,
    0x11379625747D5AF3ULL,
    0xBCE5D2248682C115ULL,
    0x9DA4243DE836994FULL,
    0x066F70B33FE09017ULL,
    0x4DC4DE189B671A1CULL,
    0x51039AB7712457C3ULL,
    0xC07A3F80C31FB4B4ULL,
    0xB46EE9C5E64A6E7CULL,
    0xB3819A42ABE61C87ULL,
    0x21A007933A522A20ULL,
    0x2DF16F761598AA4FULL,
    0x763C4A1371B368FDULL,
    0xF793C46702E086A0ULL,
    0xD7288E012AEB8D31ULL,
    0xDE336A2A4BC1C44BULL,
    0x0BF692B38D079F23ULL,
    0x2C604A7A177326B3ULL,
    0x4850E73E03EB6064ULL,
    0xCFC447F1E53C8E1BULL,
    0xB05CA3F564268D99ULL,
    0x9AE182C8BC9474E8ULL,
    0xA4FC4BD4FC5558CAULL,
    0xE755178D58FC4E76ULL,
    0x69B97DB1A4C03DFEULL,
    0xF9B5B7C4ACC67C96ULL,
    0xFC6A82D64B8655FBULL,
    0x9C684CB6C4D24417ULL,
    0x8EC97D2917456ED0ULL,
    0x6703DF9D2924E97EULL,
    0xC547F57E42A7444EULL,
    0x78E37644E7CAD29EULL,
    0xFE9A44E9362F05FAULL,
    0x08BD35CC38336615ULL,
    0x9315E5EB3A129ACEULL,
    0x94061B871E04DF75ULL,
    0xDF1D9F9D784BA010ULL,
    0x3BBA57B68871B59DULL,
    0xD2B7ADEEDED1F73FULL,
    0xF7A255D83BC373F8ULL,
    0xD7F4F2448C0CEB81ULL,
    0xD95BE88CD210FFA7ULL,
    0x336F52F8FF4728E7ULL,
    0xA74049DAC312AC71ULL,
    0xA2F61BB6E437FDB5ULL,
    0x4F2A5CB07F6A35B3ULL,
    0x87D380BDA5BF7859ULL,
    0x16B9F7E06C453A21ULL,
    0x7BA2484C8A0FD54EULL,
    0xF3A678CAD9A2E38CULL,
    0x39B0BF7DDE437BA2ULL,
    0xFCAF55C1BF8A4424ULL,
    0x18FCF680573FA594ULL,
    0x4C0563B89F495AC3ULL,
    0x40E087931A00930DULL,
    0x8CFFA9412EB642C1ULL,
    0x68CA39053261169FULL,
    0x7A1EE967D27579E2ULL,
    0x9D1D60E5076F5B6FULL,
    0x3810E399B6F65BA2ULL,
    0x32095B6D4AB5F9B1ULL,
    0x35CAB62109DD038AULL,
    0xA90B24499FCFAFB1ULL,
    0x77A225A07CC2C6BDULL,
    0x513E5E634C70E331ULL,
    0x4361C0CA3F692F12ULL,
    0xD941ACA44B20A45BULL,
    0x528F7C8602C5807BULL,
    0x52AB92BEB9613989ULL,
    0x9D1DFA2EFC557F73ULL,
    0x722FF175F572C348ULL,
    0x1D1260A51107FE97ULL,
    0x7A249A57EC0C9BA2ULL,
    0x04208FE9E8F7F2D6ULL,
    0x5A110C6058B920A0ULL,
    0x0CD9A497658A5698ULL,
    0x56FD23C8F9715A4CULL,
    0x284C847B9D887AAEULL,
    0x04FEABFBBDB619CBULL,
    0x742E1E651C60BA83ULL,
    0x9A9632E65904AD3CULL,
    0x881B82A13B51B9E2ULL,
    0x506E6744CD974924ULL,
    0xB0183DB56FFC6A79ULL,
    0x0ED9B915C66ED37EULL,
    0x5E11E86D5873D484ULL,
    0xF678647E3519AC6EULL,
    0x1B85D488D0F20CC5ULL,
    0xDAB9FE6525D89021ULL,
    0x0D151D86ADB73615ULL,
    0xA865A54EDCC0F019ULL,
    0x93C42566AEF98FFBULL,
    0x99E7AFEABE000731ULL,
    0x48CBFF086DDF285AULL,
    0x7F9B6AF1EBF78BAFULL,
    0x58627E1A149BBA21ULL,
    0x2CD16E2ABD791E33ULL,
    0xD363EFF5F0977996ULL,
    0x0CE2A38C344A6EEDULL,
    0x1A804AADB9CFA741ULL,
    0x907F30421D78C5DEULL,
    0x501F65EDB3034D07ULL,
    0x37624AE5A48FA6E9ULL,
    0x957BAF61700CFF4EULL,
    0x3A6C27934E31188AULL,
    0xD49503536ABCA345ULL,
    0x088E049589C432E0ULL,
    0xF943AEE7FEBF21B8ULL,
    0x6C3B8E3E336139D3ULL,
    0x364F6FFA464EE52EULL,
    0xD60F6DCEDC314222ULL,
    0x56963B0DCA418FC0ULL,
    0x16F50EDF91E513AFULL,
    0xEF1955914B609F93ULL,
    0x565601C0364E3228ULL,
    0xECB53939887E8175ULL,
    0xBAC7A9A18531294BULL,
    0xB344C470397BBA52ULL,
    0x65D34954DAF3CEBDULL,
    0xB4B81B3FA97511E2ULL,
    0xB422061193D6F6A7ULL,
    0x071582401C38434DULL,
    0x7A13F18BBEDC4FF5ULL,
    0xBC4097B116C524D2ULL,
    0x59B97885E2F2EA28ULL,
    0x99170A5DC3115544ULL,
    0x6F423357E7C6A9F9ULL,
    0x325928EE6E6F8794ULL,
    0xD0E4366228B03343ULL,
    0x565C31F7DE89EA27ULL,
    0x30F5611484119414ULL,
    0xD873DB391292ED4FULL,
    0x7BD94E1D8E17DEBCULL,
    0xC7D9F16864A76E94ULL,
    0x947AE053EE56E63CULL,
    0xC8C93882F9475F5FULL,
    0x3A9BF55BA91F81CAULL,
    0xD9A11FBB3D9808E4ULL,
    0x0FD22063EDC29FCAULL,
    0xB3F256D8ACA0B0B9ULL,
    0xB03031A8B4516E84ULL,
    0x35DD37D5871448AFULL,
    0xE9F6082B05542E4EULL,
    0xEBFAFA33D7254B59ULL,
    0x9255ABB50D532280ULL,
    0xB9AB4CE57F2D34F3ULL,
    0x693501D628297551ULL,
    0xC62C58F97DD949BFULL,
    0xCD454F8F19C5126AULL,
    0xBBE83F4ECC2BDECBULL,
    0xDC842B7E2819E230ULL,
    0xBA89142E007503B8ULL,
    0xA3BC941D0A5061CBULL,
    0xE9F6760E32CD8021ULL,
    0x09C7E552BC76492FULL,
    0x852F54934DA55CC9ULL,
    0x8107FCCF064FCF56ULL,
    0x098954D51FFF6580ULL,
    0x23B70EDB1955C4BFULL,
    0xC330DE426430F69DULL,
    0x4715ED43E8A45C0AULL,
    0xA8D7E4DAB780A08DULL,
    0x0572B974F03CE0BBULL,
    0xB57D2E985E1419C7ULL,
    0xE8D9ECBE2CF3D73FULL,
    0x2FE4B17170E59750ULL,
    0x11317BA87905E790ULL,
    0x7FBF21EC8A1F45ECULL,
    0x1725CABFCB045B00ULL,
    0x964E915CD5E2B207ULL,
    0x3E2B8BCBF016D66DULL,
    0xBE7444E39328A0ACULL,
    0xF85B2B4FBCDE44B7ULL,
    0x49353FEA39BA63B1ULL,
    0x1DD01AAFCD53486AULL,
    0x1FCA8A92FD719F85ULL,
    0xFC7C95D827357AFAULL,
    0x18A6A990C8B35EBDULL,
    0xCCCB7005C6B9C28DULL,
    0x3BDBB92C43B17F26ULL,
    0xAA70B5B4F89695A2ULL,
    0xE94C39A54A98307FULL,
    0xB7A0B174CFF6F36EULL,
    0xD4DBA84729AF48ADULL,
    0x2E18BC1AD9704A68ULL,
    0x2DE0966DAF2F8B1CULL,
    0xB9C11D5B1E43A07EULL,
    0x64972D68DEE33360ULL,
    0x94628D38D0C20584ULL,
    0xDBC0D2B6AB90A559ULL,
    0xD2733C4335C6A72FULL,
    0x7E75D99D94A70F4DULL,
    0x6CED1983376FA72BULL,
    0x97FCAACBF030BC24ULL,
    0x7B77497B32503B12ULL,
    0x8547EDDFB81CCB94ULL,
    0x79999CDFF70902CBULL,
    0xCFFE1939438E9B24ULL,
    0x829626E3892D95D7ULL,
    0x92FAE24291F2B3F1ULL,
    0x63E22C147B9C3403ULL,
    0xC678B6D860284A1CULL,
    0x5873888850659AE7ULL,
    0x0981DCD296A8736DULL,
    0x9F65789A6509A440ULL,
    0x9FF38FED72E9052FULL,
    0xE479EE5B9930578CULL,
    0xE7F28ECD2D49EECDULL,
    0x56C074A581EA17FEULL,
    0x5544F7D774B14AEFULL,
    0x7B3F0195FC6F290FULL,
    0x12153635B2C0CF57ULL,
    0x7F5126DBBA5E0CA7ULL,
    0x7A76956C3EAFB413ULL,
    0x3D5774A11D31AB39ULL,
    0x8A1B083821F40CB4ULL,
    0x7B4A38E32537DF62ULL,
    0x950113646D1D6E03ULL,
    0x4DA8979A0041E8A9ULL,
    0x3BC36E078F7515D7ULL,
    0x5D0A12F27AD310D1ULL,
    0x7F9D1A2E1EBE1327ULL,
    0xDA3A361B1C5157B1ULL,
    0xDCDD7D20903D0C25ULL,
    0x36833336D068F707ULL,
    0xCE68341F79893389ULL,
    0xAB9090168DD05F34ULL,
    0x43954B3252DC25E5ULL,
    0xB438C2B67F98E5E9ULL,
    0x10DCD78E3851A492ULL,
    0xDBC27AB5447822BFULL,
    0x9B3CDB65F82CA382ULL,
    0xB67B7896167B4C84ULL,
    0xBFCED1B0048EAC50ULL,
    0xA9119B60369FFEBDULL,
    0x1FFF7AC80904BF45ULL,
    0xAC12FB171817EEE7ULL,
    0xAF08DA9177DDA93DULL,
    0x1B0CAB936E65C744ULL,
    0xB559EB1D04E5E932ULL,
    0xC37B45B3F8D6F2BAULL,
    0xC3A9DC228CAAC9E9ULL,
    0xF3B8B6675A6507FFULL,
    0x9FC477DE4ED681DAULL,
    0x67378D8ECCEF96CBULL,
    0x6DD856D94D259236ULL,
    0xA319CE15B0B4DB31ULL,
    0x073973751F12DD5EULL,
    0x8A8E849EB32781A5ULL,
    0xE1925C71285279F5ULL,
    0x74C04BF1790C0EFEULL,
    0x4DDA48153C94938AULL,
    0x9D266D6A1CC0542CULL,
    0x7440FB816508C4FEULL,
    0x13328503DF48229FULL,
    0xD6BF7BAEE43CAC40ULL,
    0x4838D65F6EF6748FULL,
    0x1E152328F3318DEAULL,
    0x8F8419A348F296BFULL,
    0x72C8834A5957B511ULL,
    0xD7A023A73260B45CULL,
    0x94EBC8ABCFB56DAEULL,
    0x9FC10D0F989993E0ULL,
    0xDE68A2355B93CAE6ULL,
    0xA44CFE79AE538BBEULL,
    0x9D1D84FCCE371425ULL,
    0x51D2B1AB2DDFB636ULL,
    0x2FD7E4B9E72CD38CULL,
    0x65CA5B96B7552210ULL,
    0xDD69A0D8AB3B546DULL,
    0x604D51B25FBF70E2ULL,
    0x73AA8A564FB7AC9EULL,
    0x1A8C1E992B941148ULL,
    0xAAC40A2703D9BEA0ULL,
    0x764DBEAE7FA4F3A6ULL,
    0x1E99B96E70A9BE8BULL,
    0x2C5E9DEB57EF4743ULL,
    0x3A938FEE32D29981ULL,
    0x26E6DB8FFDF5ADFEULL,
    0x469356C504EC9F9DULL,
    0xC8763C5B08D1908CULL,
    0x3F6C6AF859D80055ULL,
    0x7F7CC39420A3A545ULL,
    0x9BFB227EBDF4C5CEULL,
    0x89039D79D6FC5C5CULL,
    0x8FE88B57305E2AB6ULL,
    0xA09E8C8C35AB96DEULL,
    0xFA7E393983325753ULL,
    0xD6B6D0ECC617C699ULL,
    0xDFEA21EA9E7557E3ULL,
    0xB67C1FA481680AF8ULL,
    0xCA1E3785A9E724E5ULL,
    0x1CFC8BED0D681639ULL,
    0xD18D8549D140CAEAULL,
    0x4ED0FE7E9DC91335ULL,
    0xE4DBF0634473F5D2ULL,
    0x1761F93A44D5AEFEULL,
    0x53898E4C3910DA55ULL,
    0x734DE8181F6EC39AULL,
    0x2680B122BAA28D97ULL,
    0x298AF231C85BAFABULL,
    0x7983EED3740847D5ULL,
    0x66C1A2A1A60CD889ULL,
    0x9E17E49642A3E4C1ULL,
    0xEDB454E7BADC0805ULL,
    0x50B704CAB602C329ULL,
    0x4CC317FB9CDDD023ULL,
    0x66B4835D9EAFEA22ULL,
    0x219B97E26FFC81BDULL,
    0x261E4E4C0A333A9DULL,
    0x1FE2CCA76517DB90ULL,
    0xD7504DFA8816EDBBULL,
    0xB9571FA04DC089C8ULL,
    0x1DDC0325259B27DEULL,
    0xCF3F4688801EB9AAULL,
    0xF4F5D05C10CAB243ULL,
    0x38B6525C21A42B0EULL,
    0x36F60E2BA4FA6800ULL,
    0xEB3593803173E0CEULL,
    0x9C4CD6257C5A3603ULL,
    0xAF0C317D32ADAA8AULL,
    0x258E5A80C7204C4BULL,
    0x8B889D624D44885DULL,
    0xF4D14597E660F855ULL,
    0xD4347F66EC8941C3ULL,
    0xE699ED85B0DFB40DULL,
    0x2472F6207C2D0484ULL,
    0xC2A1E7B5B459AEB5ULL,
    0xAB4F6451CC1D45ECULL,
    0x63767572AE3D6174ULL,
    0xA59E0BD101731A28ULL,
    0x116D0016CB948F09ULL,
    0x2CF9C8CA052F6E9FULL,
    0x0B090A7560A968E3ULL,
    0xABEEDDB2DDE06FF1ULL,
    0x58EFC10B06A2068DULL,
    0xC6E57A78FBD986E0ULL,
    0x2EAB8CA63CE802D7ULL,
    0x14A195640116F336ULL,
    0x7C0828DD624EC390ULL,
    0xD74BBE77E6116AC7ULL,
    0x804456AF10F5FB53ULL,
    0xEBE9EA2ADF4321C7ULL,
    0x03219A39EE587A30ULL,
    0x49787FEF17AF9924ULL,
    0xA1E9300CD8520548ULL,
    0x5B45E522E4B1B4EFULL,
    0xB49C3B3995091A36ULL,
    0xD4490AD526F14431ULL,
    0x12A8F216AF9418C2ULL,
    0x001F837CC7350524ULL,
    0x1877B51E57A764D5ULL,
    0xA2853B80F17F58EEULL,
    0x993E1DE72D36D310ULL,
    0xB3598080CE64A656ULL,
    0x252F59CF0D9F04BBULL,
    0xD23C8E176D113600ULL,
    0x1BDA0492E7E4586EULL,
    0x21E0BD5026C619BFULL,
    0x3B097ADAF088F94EULL,
    0x8D14DEDB30BE846EULL,
    0xF95CFFA23AF5F6F4ULL,
    0x3871700761B3F743ULL,
    0xCA672B91E9E4FA16ULL,
    0x64C8E531BFF53B55ULL,
    0x241260ED4AD1E87DULL,
    0x106C09B972D2E822ULL,
    0x7FBA195410E5CA30ULL,
    0x7884D9BC6CB569D8ULL,
    0x0647DFEDCD894A29ULL,
    0x63573FF03E224774ULL,
    0x4FC8E9560F91B123ULL,
    0x1DB956E450275779ULL,
    0xB8D91274B9E9D4FBULL,
    0xA2EBEE47E2FBFCE1ULL,
    0xD9F1F30CCD97FB09ULL,
    0xEFED53D75FD64E6BULL,
    0x2E6D02C36017F67FULL,
    0xA9AA4D20DB084E9BULL,
    0xB64BE8D8B25396C1ULL,
    0x70CB6AF7C2D5BCF0ULL,
    0x98F076A4F7A2322EULL,
    0xBF84470805E69B5FULL,
    0x94C3251F06F90CF3ULL,
    0x3E003E616A6591E9ULL,
    0xB925A6CD0421AFF3ULL,
    0x61BDD1307C66E300ULL,
    0xBF8D5108E27E0D48ULL,
    0x240AB57A8B888B20ULL,
    0xFC87614BAF287E07ULL,
    0xEF02CDD06FFDB432ULL,
    0xA1082C0466DF6C0AULL,
    0x8215E577001332C8ULL,
    0xD39BB9C3A48DB6CFULL,
    0x2738259634305C14ULL,
    0x61CF4F94C97DF93DULL,
    0x1B6BACA2AE4E125BULL,
    0x758F450C88572E0BULL,
    0x959F587D507A8359ULL,
    0xB063E962E045F54DULL,
    0x60E8ED72C0DFF5D1ULL,
    0x7B64978555326F9FULL,
    0xFD080D236DA814BAULL,
    0x8C90FD9B083F4558ULL,
    0x106F72FE81E2C590ULL,
    0x7976033A39F7D952ULL,
    0xA4EC0132764CA04BULL,
    0x733EA705FAE4FA77ULL,
    0xB4D8F77BC3E56167ULL,
    0x9E21F4F903B33FD9ULL,
    0x9D765E419FB69F6DULL,
    0xD30C088BA61EA5EFULL,
    0x5D94337FBFAF7F5BULL,
    0x1A4E4822EB4D7A59ULL,
    0x6FFE73E81B637FB3ULL,
    0xDDF957BC36D8B9CAULL,
    0x64D0E29EEA8838B3ULL,
    0x08DD9BDFD96B9F63ULL,
    0x087E79E5A57D1D13ULL,
    0xE328E230E3E2B3FBULL,
    0x1C2559E30F0946BEULL,
    0x720BF5F26F4D2EAAULL,
    0xB0774D261CC609DBULL,
    0x443F64EC5A371195ULL,
    0x4112CF68649A260EULL,
    0xD813F2FAB7F5C5CAULL,
    0x660D3257380841EEULL,
    0x59AC2C7873F910A3ULL,
    0xE846963877671A17ULL,
    0x93B633ABFA3469F8ULL,
    0xC0C0F5A60EF4CDCFULL,
    0xCAF21ECD4377B28CULL,
    0x57277707199B8175ULL,
    0x506C11B9D90E8B1DULL,
    0xD83CC2687A19255FULL,
    0x4A29C6465A314CD1ULL,
    0xED2DF21216235097ULL,
    0xB5635C95FF7296E2ULL,
    0x22AF003AB672E811ULL,
    0x52E762596BF68235ULL,
    0x9AEBA33AC6ECC6B0ULL,
    0x944F6DE09134DFB6ULL,
    0x6C47BEC883A7DE39ULL,
    0x6AD047C430A12104ULL,
    0xA5B1CFDBA0AB4067ULL,
    0x7C45D833AFF07862ULL,
    0x5092EF950A16DA0BULL,
    0x9338E69C052B8E7BULL,
    0x455A4B4CFE30E3F5ULL,
    0x6B02E63195AD0CF8ULL,
    0x6B17B224BAD6BF27ULL,
    0xD1E0CCD25BB9C169ULL,
    0xDE0C89A556B9AE70ULL,
    0x50065E535A213CF6ULL,
    0x9C1169FA2777B874ULL,
    0x78EDEFD694AF1EEDULL,
    0x6DC93D9526A50E68ULL,
    0xEE97F453F06791EDULL,
    0x32AB0EDB696703D3ULL,
    0x3A6853C7E70757A7ULL,
    0x31865CED6120F37DULL,
    0x67FEF95D92607890ULL,
    0x1F2B1D1F15F6DC9CULL,
    0xB69E38A8965C6B65ULL,
    0xAA9119FF184CCCF4ULL,
    0xF43C732873F24C13ULL,
    0xFB4A3D794A9A80D2ULL,
    0x3550C2321FD6109CULL,
    0x371F77E76BB8417EULL,
    0x6BFA9AAE5EC05779ULL,
    0xCD04F3FF001A4778ULL,
    0xE3273522064480CAULL,
    0x9F91508BFFCFC14AULL,
    0x049A7F41061A9E60ULL,
    0xFCB6BE43A9F2FE9BULL,
    0x08DE8A1C7797DA9BULL,
    0x8F9887E6078735A1ULL,
    0xB5B4071DBFC73A66ULL,
    0x230E343DFBA08D33ULL,
    0x43ED7F5A0FAE657DULL,
    0x3A88A0FBBCB05C63ULL,
    0x21874B8B4D2DBC4FULL,
    0x1BDEA12E35F6A8C9ULL,
    0x53C065C6C8E63528ULL,
    0xE34A1D250E7A8D6BULL,
    0xD6B04D3B7651DD7EULL,
    0x5E90277E7CB39E2DULL,
    0x2C046F22062DC67DULL,
    0xB10BB459132D0A26ULL,
    0x3FA9DDFB67E2F199ULL,
    0x0E09B88E1914F7AFULL,
    0x10E8B35AF3EEAB37ULL,
    0x9EEDECA8E272B933ULL,
    0xD4C718BC4AE8AE5FULL,
    0x81536D601170FC20ULL,
    0x91B534F885818A06ULL,
    0xEC8177F83F900978ULL,
    0x190E714FADA5156EULL,
    0xB592BF39B0364963ULL,
    0x89C350C893AE7DC1ULL,
    0xAC042E70F8B383F2ULL,
    0xB49B52E587A1EE60ULL,
    0xFB152FE3FF26DA89ULL,
    0x3E666E6F69AE2C15ULL,
    0x3B544EBE544C19F9ULL,
    0xE805A1E290CF2456ULL,
    0x24B33C9D7ED25117ULL,
    0xE74733427B72F0C1ULL,
    0x0A804D18B7097475ULL,
    0x57E3306D881EDB4FULL,
    0x4AE7D6A36EB5DBCBULL,
    0x2D8D5432157064C8ULL,
    0xD1E649DE1E7F268BULL,
    0x8A328A1CEDFE552CULL,
    0x07A3AEC79624C7DAULL,
    0x84547DDC3E203C94ULL,
    0x990A98FD5071D263ULL,
    0x1A4FF12616EEFC89ULL,
    0xF6F7FD1431714200ULL,
    0x30C05B1BA332F41CULL,
    0x8D2636B81555A786ULL,
    0x46C9FEB55D120902ULL,
    0xCCEC0A73B49C9921ULL,
    0x4E9D2827355FC492ULL,
    0x19EBB029435DCB0FULL,
    0x4659D2B743848A2CULL,
    0x963EF2C96B33BE31ULL,
    0x74F85198B05A2E7DULL,
    0x5A0F544DD2B1FB18ULL,
    0x03727073C2E134B1ULL,
    0xC7F6AA2DE59AEA61ULL,
    0x352787BAA0D7C22FULL,
    0x9853EAB63B5E0B35ULL,
    0xABBDCDD7ED5C0860ULL,
    0xCF05DAF5AC8D77B0ULL,
    0x49CAD48CEBF4A71EULL,
    0x7A4C10EC2158C4A6ULL,
    0xD9E92AA246BF719EULL,
    0x13AE978D09FE5557ULL,
    0x730499AF921549FFULL,
    0x4E4B705B92903BA4ULL,
    0xFF577222C14F0A3AULL,
    0x55B6344CF97AAFAEULL,
    0xB862225B055B6960ULL,
    0xCAC09AFBDDD2CDB4ULL,
    0xDAF8E9829FE96B5FULL,
    0xB5FDFC5D3132C498ULL,
    0x310CB380DB6F7503ULL,
    0xE87FBB46217A360EULL,
    0x2102AE466EBB1148ULL,
    0xF8549E1A3AA5E00DULL,
    0x07A69AFDCC42261AULL,
    0xC4C118BFE78FEAAEULL,
    0xF9F4892ED96BD438ULL,
    0x1AF3DBE25D8F45DAULL,
    0xF5B4B0B0D2DEEEB4ULL,
    0x962ACEEFA82E1C84ULL,
    0x046E3ECAAF453CE9ULL,
    0xF05D129681949A4CULL,
    0x964781CE734B3C84ULL,
    0x9C2ED44081CE5FBDULL,
    0x522E23F3925E319EULL,
    0x177E00F9FC32F791ULL,
    0x2BC60A63A6F3B3F2ULL,
    0x222BBFAE61725606ULL,
    0x486289DDCC3D6780ULL,
    0x7DC7785B8EFDFC80ULL,
    0x8AF38731C02BA980ULL,
    0x1FAB64EA29A2DDF7ULL,
    0xE4D9429322CD065AULL,
    0x9DA058C67844F20CULL,
    0x24C0E332B70019B0ULL,
    0x233003B5A6CFE6ADULL,
    0xD586BD01C5C217F6ULL,
    0x5E5637885F29BC2BULL,
    0x7EBA726D8C94094BULL,
    0x0A56A5F0BFE39272ULL,
    0xD79476A84EE20D06ULL,
    0x9E4C1269BAA4BF37ULL,
    0x17EFEE45B0DEE640ULL,
    0x1D95B0A5FCF90BC6ULL,
    0x93CBE0B699C2585DULL,
    0x65FA4F227A2B6D79ULL,
    0xD5F9E858292504D5ULL,
    0xC2B5A03F71471A6FULL,
    0x59300222B4561E00ULL,
    0xCE2F8642CA0712DCULL,
    0x7CA9723FBB2E8988ULL,
    0x2785338347F2BA08ULL,
    0xC61BB3A141E50E8CULL,
    0x150F361DAB9DEC26ULL,
    0x9F6A419D382595F4ULL,
    0x64A53DC924FE7AC9ULL,
    0x142DE49FFF7A7C3DULL,
    0x0C335248857FA9E7ULL,
    0x0A9C32D5EAE45305ULL,
    0xE6C42178C4BBB92EULL,
    0x71F1CE2490D20B07ULL,
    0xF1BCC3D275AFE51AULL,
    0xE728E8C83C334074ULL,
    0x96FBF83A12884624ULL,
    0x81A1549FD6573DA5ULL,
    0x5FA7867CAF35E149ULL,
    0x56986E2EF3ED091BULL,
    0x917F1DD5F8886C61ULL,
    0xD20D8C88C8FFE65FULL,
    0x31D71DCE64B2C310ULL,
    0xF165B587DF898190ULL,
    0xA57E6339DD2CF3A0ULL,
    0x1EF6E6DBB1961EC9ULL,
    0x70CC73D90BC26E24ULL,
    0xE21A6B35DF0C3AD7ULL,
    0x003A93D8B2806962ULL,
    0x1C99DED33CB890A1ULL,
    0xCF3145DE0ADD4289ULL,
    0xD0E4427A5514FB72ULL,
    0x77C621CC9FB3A483ULL,
    0x67A34DAC4356550BULL,
    0xF8D626AAAF278509ULL,
};

struct FenLayout {
    std::array<char, 64> squares{};
    int white_king_square = -1;
    int black_king_square = -1;
    int white_kings = 0;
    int black_kings = 0;
    int white_pawns = 0;
    int black_pawns = 0;
    int white_pieces = 0;
    int black_pieces = 0;
};

bool split_fen_fields(std::string_view fen, std::array<std::string_view, 6>& fields) {
    std::size_t offset = 0;
    std::size_t count = 0;
    while (offset < fen.size()) {
        while (offset < fen.size() && std::isspace(static_cast<unsigned char>(fen[offset]))) {
            ++offset;
        }
        if (offset == fen.size()) {
            break;
        }
        if (count == fields.size()) {
            return false;
        }

        const std::size_t start = offset;
        while (offset < fen.size() && !std::isspace(static_cast<unsigned char>(fen[offset]))) {
            ++offset;
        }
        fields[count++] = fen.substr(start, offset - start);
    }
    return count == fields.size();
}

bool valid_piece_placement(std::string_view placement, FenLayout& layout) {
    layout = {};
    int rank = 7;
    int width = 0;
    for (char character : placement) {
        if (character == '/') {
            if (width != 8 || rank == 0) {
                return false;
            }
            --rank;
            width = 0;
        } else if (character >= '1' && character <= '8') {
            width += character - '0';
        } else if (std::string_view("PNBRQKpnbrqk").contains(character)) {
            if (width >= 8 || rank < 0) {
                return false;
            }
            if ((character == 'P' || character == 'p') && (rank == 0 || rank == 7)) {
                return false;
            }
            const int square = rank * 8 + width;
            layout.squares[static_cast<std::size_t>(square)] = character;
            if (character == 'K') {
                layout.white_king_square = square;
                ++layout.white_kings;
            } else if (character == 'k') {
                layout.black_king_square = square;
                ++layout.black_kings;
            }
            if (std::isupper(static_cast<unsigned char>(character))) {
                ++layout.white_pieces;
                if (character == 'P') ++layout.white_pawns;
            } else {
                ++layout.black_pieces;
                if (character == 'p') ++layout.black_pawns;
            }
            ++width;
        } else {
            return false;
        }
        if (width > 8) {
            return false;
        }
    }

    if (rank != 0 || width != 8 || layout.white_kings != 1 || layout.black_kings != 1 ||
        layout.white_pawns > 8 || layout.black_pawns > 8 ||
        layout.white_pieces > 16 || layout.black_pieces > 16) {
        return false;
    }

    const int white_rank = layout.white_king_square / 8;
    const int white_file = layout.white_king_square % 8;
    const int black_rank = layout.black_king_square / 8;
    const int black_file = layout.black_king_square % 8;
    const int rank_distance = white_rank - black_rank;
    const int file_distance = white_file - black_file;
    const bool adjacent_kings = rank_distance >= -1 && rank_distance <= 1 && file_distance >= -1 &&
                                file_distance <= 1;
    return !adjacent_kings;
}

bool valid_castling(std::string_view castling, const FenLayout& layout) {
    if (castling == "-") {
        return true;
    }
    if (castling.empty() || castling.size() > 4) {
        return false;
    }
    for (std::size_t index = 0; index < castling.size(); ++index) {
        if (!std::string_view("KQkq").contains(castling[index]) ||
            castling.find(castling[index], index + 1) != std::string_view::npos) {
            return false;
        }
    }

    const auto piece_at = [&layout](int square) { return layout.squares[static_cast<std::size_t>(square)]; };
    return (!castling.contains('K') || (piece_at(4) == 'K' && piece_at(7) == 'R')) &&
           (!castling.contains('Q') || (piece_at(4) == 'K' && piece_at(0) == 'R')) &&
           (!castling.contains('k') || (piece_at(60) == 'k' && piece_at(63) == 'r')) &&
           (!castling.contains('q') || (piece_at(60) == 'k' && piece_at(56) == 'r'));
}

bool valid_en_passant(std::string_view en_passant, std::string_view side_to_move,
                      std::string_view halfmove_clock, const FenLayout& layout) {
    if (en_passant == "-") {
        return true;
    }
    if (en_passant.size() != 2 || en_passant[0] < 'a' || en_passant[0] > 'h' ||
        !((side_to_move == "w" && en_passant[1] == '6') || (side_to_move == "b" && en_passant[1] == '3')) ||
        halfmove_clock != "0") {
        return false;
    }

    const int target_file = en_passant[0] - 'a';
    const int target_rank = en_passant[1] - '1';
    const int target_square = target_rank * 8 + target_file;
    const int pawn_square = (side_to_move == "w" ? target_rank - 1 : target_rank + 1) * 8 + target_file;
    const int origin_square = (side_to_move == "w" ? target_rank + 1 : target_rank - 1) * 8 + target_file;
    const char pawn = side_to_move == "w" ? 'p' : 'P';
    return layout.squares[static_cast<std::size_t>(target_square)] == '\0' &&
           layout.squares[static_cast<std::size_t>(pawn_square)] == pawn &&
           layout.squares[static_cast<std::size_t>(origin_square)] == '\0';
}

bool valid_counter(std::string_view counter, bool allow_zero, std::uint32_t maximum) {
    std::uint32_t value = 0;
    const auto [end, error] = std::from_chars(counter.data(), counter.data() + counter.size(), value);
    return error == std::errc{} && end == counter.data() + counter.size() && (allow_zero || value > 0) &&
           value <= maximum;
}

bool valid_fen_syntax(std::string_view fen) {
    std::array<std::string_view, 6> fields{};
    FenLayout layout;
    return split_fen_fields(fen, fields) && valid_piece_placement(fields[0], layout) &&
           (fields[1] == "w" || fields[1] == "b") && valid_castling(fields[2], layout) &&
           valid_en_passant(fields[3], fields[1], fields[4], layout) && valid_counter(fields[4], true, 255) &&
           valid_counter(fields[5], false, 32768);
}

std::vector<std::string> sorted_native_move_strings(const Position& position) {
    std::vector<std::string> moves;
    for (const Move& move : position.legal_moves()) {
        moves.push_back(move.uci());
    }
    std::sort(moves.begin(), moves.end());
    return moves;
}

std::uint64_t metadata_validation_token(const std::uint64_t key,
                                        const MoveMetadata& metadata) noexcept {
    std::uint64_t value = key ^ 0x9E3779B97F4A7C15ULL;
    value ^= static_cast<std::uint64_t>(metadata.move.from().index()) << 1U;
    value ^= static_cast<std::uint64_t>(metadata.move.to().index()) << 8U;
    value ^= static_cast<std::uint64_t>(metadata.move.promotion()) << 15U;
    value ^= static_cast<std::uint64_t>(metadata.moving_piece) << 20U;
    value ^= static_cast<std::uint64_t>(metadata.captured_piece) << 24U;
    value ^= static_cast<std::uint64_t>(metadata.kind) << 28U;
    value ^= static_cast<std::uint64_t>(metadata.gives_check) << 32U;
    value ^= value >> 30U;
    value *= 0xBF58476D1CE4E5B9ULL;
    value ^= value >> 27U;
    value *= 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
}

constexpr int exchange_piece_value(PieceType type) noexcept {
    switch (type) {
    case PieceType::pawn:
        return 100;
    case PieceType::knight:
        return 320;
    case PieceType::bishop:
        return 330;
    case PieceType::rook:
        return 500;
    case PieceType::queen:
        return 900;
    case PieceType::king:
        return 20'000;
    case PieceType::none:
        return 0;
    }
    return 0;
}

constexpr PieceType promotion_piece_type(Promotion promotion) noexcept {
    switch (promotion) {
    case Promotion::knight:
        return PieceType::knight;
    case Promotion::bishop:
        return PieceType::bishop;
    case Promotion::rook:
        return PieceType::rook;
    case Promotion::queen:
        return PieceType::queen;
    case Promotion::none:
        return PieceType::pawn;
    }
    return PieceType::pawn;
}

constexpr int promotion_exchange_gain(Promotion promotion) noexcept {
    return exchange_piece_value(promotion_piece_type(promotion)) - exchange_piece_value(PieceType::pawn);
}

constexpr std::uint64_t kCoreCenterMask =
    (std::uint64_t{1} << 27) | (std::uint64_t{1} << 28) |
    (std::uint64_t{1} << 35) | (std::uint64_t{1} << 36);

std::uint64_t king_zone_mask(std::uint8_t square) noexcept {
    if (square >= Square::kInvalid) {
        return 0;
    }
    const int file = square % 8;
    const int rank = square / 8;
    std::uint64_t mask = 0;
    for (int rank_delta = -1; rank_delta <= 1; ++rank_delta) {
        for (int file_delta = -1; file_delta <= 1; ++file_delta) {
            const int target_file = file + file_delta;
            const int target_rank = rank + rank_delta;
            if (target_file >= 0 && target_file < 8 && target_rank >= 0 && target_rank < 8) {
                mask |= std::uint64_t{1} << (target_rank * 8 + target_file);
            }
        }
    }
    return mask;
}

using NativeFeatureBitboards = std::array<std::array<std::uint64_t, 7>, 2>;

constexpr std::uint64_t feature_bit(int square) noexcept {
    return square >= 0 && square < 64 ? std::uint64_t{1} << square : 0;
}

std::uint64_t native_feature_attacks(const NativeFeatureBitboards& pieces,
                                     std::uint64_t occupied, int source,
                                     PieceType type, Color color) noexcept {
    const int file = source % 8;
    const int rank = source / 8;
    std::uint64_t attacks = 0;

    if (type == PieceType::pawn) {
        const int direction = color == Color::white ? 1 : -1;
        for (const int file_delta : {-1, 1}) {
            const int target_file = file + file_delta;
            const int target_rank = rank + direction;
            if (target_file >= 0 && target_file < 8 && target_rank >= 0 && target_rank < 8) {
                attacks |= feature_bit(target_rank * 8 + target_file);
            }
        }
        return attacks;
    }

    if (type == PieceType::knight || type == PieceType::king) {
        constexpr int knight_directions[8][2] = {
            {1, 2}, {2, 1}, {2, -1}, {1, -2},
            {-1, -2}, {-2, -1}, {-2, 1}, {-1, 2},
        };
        constexpr int king_directions[8][2] = {
            {1, 1}, {1, 0}, {1, -1}, {0, 1},
            {0, -1}, {-1, 1}, {-1, 0}, {-1, -1},
        };
        const auto& directions = type == PieceType::knight ? knight_directions : king_directions;
        for (const auto& direction : directions) {
            const int target_file = file + direction[0];
            const int target_rank = rank + direction[1];
            if (target_file >= 0 && target_file < 8 && target_rank >= 0 && target_rank < 8) {
                attacks |= feature_bit(target_rank * 8 + target_file);
            }
        }
        return attacks;
    }

    constexpr int bishop_directions[4][2] = {
        {1, 1}, {1, -1}, {-1, 1}, {-1, -1},
    };
    constexpr int rook_directions[4][2] = {
        {1, 0}, {-1, 0}, {0, 1}, {0, -1},
    };
    constexpr int queen_directions[8][2] = {
        {1, 1}, {1, -1}, {-1, 1}, {-1, -1},
        {1, 0}, {-1, 0}, {0, 1}, {0, -1},
    };
    const auto& directions = type == PieceType::bishop ? bishop_directions :
        (type == PieceType::rook ? rook_directions : queen_directions);
    const int direction_count = type == PieceType::queen ? 8 : 4;
    for (int direction = 0; direction < direction_count; ++direction) {
        int target_file = file + directions[direction][0];
        int target_rank = rank + directions[direction][1];
        while (target_file >= 0 && target_file < 8 && target_rank >= 0 && target_rank < 8) {
            const std::uint64_t target = feature_bit(target_rank * 8 + target_file);
            attacks |= target;
            if ((occupied & target) != 0) {
                break;
            }
            target_file += directions[direction][0];
            target_rank += directions[direction][1];
        }
    }
    (void)pieces;
    return attacks;
}

bool native_move_gives_check(const Position& position, const Move& move) noexcept {
    const std::uint8_t source = move.from().index();
    const std::uint8_t destination = move.to().index();
    if (source >= Square::kInvalid || destination >= Square::kInvalid) {
        return false;
    }

    const Color moving_color = position.side_to_move();
    const Color enemy_color = opposite(moving_color);
    const std::uint64_t enemy_king = position.piece_bitboard(PieceType::king, enemy_color);
    if (enemy_king == 0) {
        return false;
    }

    std::uint64_t occupied = 0;
    for (const Color color : {Color::white, Color::black}) {
        for (const PieceType type : {PieceType::pawn, PieceType::knight, PieceType::bishop,
                                     PieceType::rook, PieceType::queen, PieceType::king}) {
            occupied |= position.piece_bitboard(type, color);
        }
    }

    const std::uint64_t source_bit = std::uint64_t{1} << source;
    const std::uint64_t destination_bit = std::uint64_t{1} << destination;
    occupied &= ~source_bit;
    if (!position.piece_at(Square::from_index(destination)).empty()) {
        occupied &= ~destination_bit;
    }

    const Piece moving_piece = position.piece_at(Square::from_index(source));
    if (moving_piece.type == PieceType::pawn &&
        position.en_passant_square().index() == destination &&
        position.piece_at(Square::from_index(destination)).empty()) {
        const int captured_square = static_cast<int>(destination) +
            (moving_color == Color::white ? -8 : 8);
        if (captured_square >= 0 && captured_square < 64) {
            occupied &= ~(std::uint64_t{1} << captured_square);
        }
    }
    occupied |= destination_bit;

    const bool castling = moving_piece.type == PieceType::king &&
        std::abs(static_cast<int>(destination % 8) - static_cast<int>(source % 8)) == 2;
    std::uint8_t rook_source = Square::kInvalid;
    std::uint8_t rook_destination = Square::kInvalid;
    if (castling) {
        const bool king_side = destination > source;
        rook_source = static_cast<std::uint8_t>((source / 8) * 8 + (king_side ? 7 : 0));
        rook_destination = static_cast<std::uint8_t>((source / 8) * 8 + (king_side ? 5 : 3));
        occupied &= ~(std::uint64_t{1} << rook_source);
        occupied |= std::uint64_t{1} << rook_destination;
    }

    NativeFeatureBitboards empty_pieces{};
    const PieceType moved_type = move.promotion() == Promotion::none ? moving_piece.type :
        promotion_piece_type(move.promotion());
    if ((native_feature_attacks(empty_pieces, occupied, destination, moved_type, moving_color) &
         enemy_king) != 0) {
        return true;
    }

    for (const PieceType type : {PieceType::pawn, PieceType::knight, PieceType::bishop,
                                 PieceType::rook, PieceType::queen, PieceType::king}) {
        std::uint64_t remaining = position.piece_bitboard(type, moving_color);
        while (remaining != 0) {
            const int square = static_cast<int>(std::countr_zero(remaining));
            remaining &= remaining - 1;
            if (square == source || (castling && square == rook_source)) {
                continue;
            }
            if ((native_feature_attacks(empty_pieces, occupied, square, type, moving_color) &
                 enemy_king) != 0) {
                return true;
            }
        }
    }

    if (castling && (native_feature_attacks(empty_pieces, occupied, rook_destination,
                                            PieceType::rook, moving_color) & enemy_king) != 0) {
        return true;
    }
    return false;
}

PositionFeatures native_position_features(const Position& position) noexcept {
    NativeFeatureBitboards pieces{};
    for (const Color color : {Color::white, Color::black}) {
        const std::size_t color_index = color == Color::white ? 0U : 1U;
        for (const PieceType type : {PieceType::pawn, PieceType::knight, PieceType::bishop,
                                     PieceType::rook, PieceType::queen, PieceType::king}) {
            pieces[color_index][static_cast<std::size_t>(type)] =
                position.piece_bitboard(type, color);
        }
    }

    PositionFeatures features;
    features.side_to_move = position.side_to_move();
    features.fullmove_number = position.fullmove_number();
    const std::uint64_t occupied =
        std::accumulate(pieces[0].begin(), pieces[0].end(), std::uint64_t{0},
                        [](std::uint64_t value, std::uint64_t board) { return value | board; }) |
        std::accumulate(pieces[1].begin(), pieces[1].end(), std::uint64_t{0},
                        [](std::uint64_t value, std::uint64_t board) { return value | board; });
    std::array<std::uint64_t, 2> attacks{};

    for (const Color color : {Color::white, Color::black}) {
        const std::size_t color_index = color == Color::white ? 0U : 1U;
        const std::uint64_t own = std::accumulate(
            pieces[color_index].begin(), pieces[color_index].end(), std::uint64_t{0},
            [](std::uint64_t value, std::uint64_t board) { return value | board; });
        for (const PieceType type : {PieceType::pawn, PieceType::knight, PieceType::bishop,
                                     PieceType::rook, PieceType::queen, PieceType::king}) {
            std::uint64_t remaining = pieces[color_index][static_cast<std::size_t>(type)];
            while (remaining != 0) {
                const int square = static_cast<int>(std::countr_zero(remaining));
                remaining &= remaining - 1;
                features.board[static_cast<std::size_t>(square)] = {type, color};
                if (type == PieceType::pawn) {
                    features.pawn_file_masks[color_index] = static_cast<std::uint8_t>(
                        features.pawn_file_masks[color_index] |
                        (std::uint8_t{1} << (square % 8)));
                }
                if (type == PieceType::knight || type == PieceType::bishop) {
                    const bool white_home = color == Color::white &&
                        ((type == PieceType::knight && (square == 1 || square == 6)) ||
                         (type == PieceType::bishop && (square == 2 || square == 5)));
                    const bool black_home = color == Color::black &&
                        ((type == PieceType::knight && (square == 57 || square == 62)) ||
                         (type == PieceType::bishop && (square == 58 || square == 61)));
                    if (!white_home && !black_home) {
                        ++features.development[color_index];
                    }
                }
                switch (type) {
                case PieceType::knight:
                case PieceType::bishop:
                    features.game_phase = static_cast<std::uint8_t>(features.game_phase + 1);
                    break;
                case PieceType::rook:
                    features.game_phase = static_cast<std::uint8_t>(features.game_phase + 2);
                    break;
                case PieceType::queen:
                    features.game_phase = static_cast<std::uint8_t>(features.game_phase + 4);
                    break;
                default:
                    break;
                }
                attacks[color_index] |= native_feature_attacks(pieces, occupied, square, type, color);
            }
        }
        features.attacked_squares[color_index] = attacks[color_index];
        features.mobility[color_index] = static_cast<std::uint16_t>(
            std::popcount(attacks[color_index] & ~own));
        const std::uint64_t king = pieces[color_index][static_cast<std::size_t>(PieceType::king)];
        const std::uint8_t king_square = king == 0 ? Square::kInvalid :
            static_cast<std::uint8_t>(std::countr_zero(king));
        features.king_squares[color_index] = Square::from_index(king_square);
        features.center_control[color_index] = static_cast<std::uint8_t>(
            std::popcount(attacks[color_index] & kCoreCenterMask));
    }

    features.game_phase = std::min<std::uint8_t>(features.game_phase, 24);
    for (std::size_t color = 0; color < 2; ++color) {
        features.king_zone_attacks[color] = static_cast<std::uint8_t>(std::popcount(
            attacks[1 - color] & king_zone_mask(features.king_squares[color].index())));
    }
    features.castling_rights = position.castling_rights();
    return features;
}

constexpr bool exchange_square_valid(int square) noexcept {
    return square >= 0 && square < 64;
}

bool exchange_square_attacked_by(const std::array<Piece, 64>& board, int target, Color attacker) noexcept {
    if (!exchange_square_valid(target)) {
        return false;
    }

    const int target_file = target & 7;
    const int target_rank = target >> 3;
    const auto has_piece = [&board, attacker](int square, PieceType type) noexcept {
        if (!exchange_square_valid(square)) {
            return false;
        }
        const Piece piece = board[static_cast<std::size_t>(square)];
        return piece.color == attacker && piece.type == type;
    };

    const int pawn_rank = target_rank - (attacker == Color::white ? 1 : -1);
    if (pawn_rank >= 0 && pawn_rank < 8) {
        if ((target_file > 0 && has_piece((pawn_rank << 3) + target_file - 1, PieceType::pawn)) ||
            (target_file < 7 && has_piece((pawn_rank << 3) + target_file + 1, PieceType::pawn))) {
            return true;
        }
    }

    constexpr int knight_directions[8][2] = {
        {1, 2}, {2, 1}, {2, -1}, {1, -2},
        {-1, -2}, {-2, -1}, {-2, 1}, {-1, 2},
    };
    for (const auto& direction : knight_directions) {
        const int file = target_file + direction[0];
        const int rank = target_rank + direction[1];
        if (file >= 0 && file < 8 && rank >= 0 && rank < 8 &&
            has_piece((rank << 3) + file, PieceType::knight)) {
            return true;
        }
    }

    for (int file = target_file - 1; file <= target_file + 1; ++file) {
        for (int rank = target_rank - 1; rank <= target_rank + 1; ++rank) {
            if ((file == target_file && rank == target_rank) ||
                file < 0 || file >= 8 || rank < 0 || rank >= 8) {
                continue;
            }
            if (has_piece((rank << 3) + file, PieceType::king)) {
                return true;
            }
        }
    }

    constexpr int directions[8][2] = {
        {1, 0}, {-1, 0}, {0, 1}, {0, -1},
        {1, 1}, {1, -1}, {-1, 1}, {-1, -1},
    };
    for (int direction = 0; direction < 8; ++direction) {
        const bool diagonal = direction >= 4;
        int file = target_file + directions[direction][0];
        int rank = target_rank + directions[direction][1];
        while (file >= 0 && file < 8 && rank >= 0 && rank < 8) {
            const Piece piece = board[static_cast<std::size_t>((rank << 3) + file)];
            if (!piece.empty()) {
                if (piece.color == attacker &&
                    ((diagonal && (piece.type == PieceType::bishop || piece.type == PieceType::queen)) ||
                     (!diagonal && (piece.type == PieceType::rook || piece.type == PieceType::queen)))) {
                    return true;
                }
                break;
            }
            file += directions[direction][0];
            rank += directions[direction][1];
        }
    }
    return false;
}

template <typename CandidateHandler>
void for_each_exchange_attacker(const std::array<Piece, 64>& board, int target,
                                Color attacker, CandidateHandler&& handler) noexcept {
    if (!exchange_square_valid(target)) {
        return;
    }
    const int target_file = target & 7;
    const int target_rank = target >> 3;
    const auto visit = [&board, attacker, &handler](int square, PieceType expected) noexcept {
        if (exchange_square_valid(square) &&
            board[static_cast<std::size_t>(square)].color == attacker &&
            board[static_cast<std::size_t>(square)].type == expected) {
            handler(square);
        }
    };

    const int pawn_rank = target_rank - (attacker == Color::white ? 1 : -1);
    if (pawn_rank >= 0 && pawn_rank < 8) {
        if (target_file > 0) visit((pawn_rank << 3) + target_file - 1, PieceType::pawn);
        if (target_file < 7) visit((pawn_rank << 3) + target_file + 1, PieceType::pawn);
    }

    constexpr int knight_directions[8][2] = {
        {1, 2}, {2, 1}, {2, -1}, {1, -2},
        {-1, -2}, {-2, -1}, {-2, 1}, {-1, 2},
    };
    for (const auto& direction : knight_directions) {
        const int file = target_file + direction[0];
        const int rank = target_rank + direction[1];
        if (file >= 0 && file < 8 && rank >= 0 && rank < 8) {
            visit((rank << 3) + file, PieceType::knight);
        }
    }

    for (int file = target_file - 1; file <= target_file + 1; ++file) {
        for (int rank = target_rank - 1; rank <= target_rank + 1; ++rank) {
            if ((file == target_file && rank == target_rank) ||
                file < 0 || file >= 8 || rank < 0 || rank >= 8) {
                continue;
            }
            visit((rank << 3) + file, PieceType::king);
        }
    }

    constexpr int directions[8][2] = {
        {1, 0}, {-1, 0}, {0, 1}, {0, -1},
        {1, 1}, {1, -1}, {-1, 1}, {-1, -1},
    };
    for (int direction = 0; direction < 8; ++direction) {
        const bool diagonal = direction >= 4;
        int file = target_file + directions[direction][0];
        int rank = target_rank + directions[direction][1];
        while (file >= 0 && file < 8 && rank >= 0 && rank < 8) {
            const int square = (rank << 3) + file;
            const Piece piece = board[static_cast<std::size_t>(square)];
            if (!piece.empty()) {
                if (piece.color == attacker &&
                    ((diagonal && (piece.type == PieceType::bishop || piece.type == PieceType::queen)) ||
                     (!diagonal && (piece.type == PieceType::rook || piece.type == PieceType::queen)))) {
                    handler(square);
                }
                break;
            }
            file += directions[direction][0];
            rank += directions[direction][1];
        }
    }
}

bool exchange_recapture_is_legal(std::array<Piece, 64>& board, std::array<int, 2>& king_squares,
                                  Color side, int source, int target, Piece placed_piece) noexcept {
    const Piece moving_piece = board[static_cast<std::size_t>(source)];
    const Piece captured_piece = board[static_cast<std::size_t>(target)];
    const std::size_t side_index = side == Color::white ? 0 : 1;
    const int saved_king_square = king_squares[side_index];

    board[static_cast<std::size_t>(source)] = {};
    board[static_cast<std::size_t>(target)] = placed_piece;
    if (moving_piece.type == PieceType::king) {
        king_squares[side_index] = target;
    }
    const bool legal = !exchange_square_attacked_by(board, king_squares[side_index], opposite(side));
    board[static_cast<std::size_t>(source)] = moving_piece;
    board[static_cast<std::size_t>(target)] = captured_piece;
    king_squares[side_index] = saved_king_square;
    return legal;
}

int static_exchange_gain_from_features(const PositionFeatures& features,
                                       const MoveMetadata& initial) noexcept {
    const Move move = initial.move;
    if (move.is_no_move() || move.from().index() == Square::kInvalid ||
        move.to().index() == Square::kInvalid ||
        (!initial.is_capture() && initial.captured_piece == PieceType::none)) {
        return 0;
    }

    std::array<Piece, 64> board = features.board;
    std::array<int, 2> king_squares{
        static_cast<int>(features.king_squares[0].index()),
        static_cast<int>(features.king_squares[1].index()),
    };
    const int source = move.from().index();
    const int target = move.to().index();
    if (!exchange_square_valid(source) || !exchange_square_valid(target)) {
        return 0;
    }

    const Piece moving_piece = board[static_cast<std::size_t>(source)];
    if (moving_piece.empty() || moving_piece.color != features.side_to_move ||
        moving_piece.type != initial.moving_piece || king_squares[0] < 0 || king_squares[1] < 0) {
        return 0;
    }

    int captured_square = target;
    if (initial.kind == MoveKind::en_passant) {
        captured_square += moving_piece.color == Color::white ? -8 : 8;
    }
    if (!exchange_square_valid(captured_square)) {
        return 0;
    }

    constexpr std::size_t kMaximumExchangeDepth = 32;
    std::array<int, kMaximumExchangeDepth> gains{};
    const Piece captured_piece = board[static_cast<std::size_t>(captured_square)];
    gains[0] = exchange_piece_value(captured_piece.type) + promotion_exchange_gain(move.promotion());

    Piece placed_piece = moving_piece;
    if (move.promotion() != Promotion::none) {
        placed_piece.type = promotion_piece_type(move.promotion());
    }
    board[static_cast<std::size_t>(source)] = {};
    board[static_cast<std::size_t>(captured_square)] = {};
    board[static_cast<std::size_t>(target)] = placed_piece;
    if (moving_piece.type == PieceType::king) {
        king_squares[moving_piece.color == Color::white ? 0 : 1] = target;
    }

    Color side = opposite(moving_piece.color);
    std::size_t depth = 0;
    for (;;) {
        int attacker_square = -1;
        int attacker_value = std::numeric_limits<int>::max();
        Piece recapturing_piece{};
        int recapture_promotion_gain = 0;

        for_each_exchange_attacker(board, target, side, [&](const int square) noexcept {
            const Piece candidate = board[static_cast<std::size_t>(square)];
            Piece replacement = candidate;
            int candidate_promotion_gain = 0;
            if (candidate.type == PieceType::pawn && (target >> 3 == 0 || target >> 3 == 7)) {
                replacement.type = PieceType::queen;
                candidate_promotion_gain =
                    exchange_piece_value(PieceType::queen) - exchange_piece_value(PieceType::pawn);
            }
            if (!exchange_recapture_is_legal(board, king_squares, side, square, target, replacement)) {
                return;
            }

            const int value = exchange_piece_value(candidate.type);
            if (value < attacker_value ||
                (value == attacker_value && (attacker_square < 0 || square < attacker_square))) {
                attacker_square = square;
                attacker_value = value;
                recapturing_piece = replacement;
                recapture_promotion_gain = candidate_promotion_gain;
            }
        });

        if (attacker_square < 0 || depth + 1 >= kMaximumExchangeDepth) {
            break;
        }

        const Piece target_piece = board[static_cast<std::size_t>(target)];
        ++depth;
        gains[depth] = exchange_piece_value(target_piece.type) - gains[depth - 1] +
            recapture_promotion_gain;

        const Piece departing_piece = board[static_cast<std::size_t>(attacker_square)];
        board[static_cast<std::size_t>(attacker_square)] = {};
        board[static_cast<std::size_t>(target)] = recapturing_piece;
        if (departing_piece.type == PieceType::king) {
            king_squares[side == Color::white ? 0 : 1] = target;
        }
        side = opposite(side);
    }

    while (depth > 0) {
        gains[depth - 1] = -std::max(-gains[depth - 1], gains[depth]);
        --depth;
    }
    return gains[0];
}

} // namespace

class GameState::Impl {
public:
    // NativePosition is the legality and incremental-key authority. The
    // vendored board is owned by CompatibilityMirror and remains a private
    // compatibility adapter for Polyglot and diagnostic consumers.
    detail::CompatibilityMirror compatibility_mirror{};
    Position native_position{};
    mutable detail::FeatureState feature_state{};
    mutable std::atomic_uint64_t check_flag_evaluations = 0;

    Impl() = default;

    Impl(const Impl& other) : compatibility_mirror(other.compatibility_mirror),
                              native_position(other.native_position),
                              feature_state(other.feature_state,
                                            other.compatibility_mirror.history_size()) {
        check_flag_evaluations.store(
            other.check_flag_evaluations.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
    }
};

GameState::GameState() : impl_(std::make_unique<Impl>()) {}

GameState::GameState(const GameState& other) : impl_(std::make_unique<Impl>(*other.impl_)) {}

GameState::GameState(GameState&&) noexcept = default;

GameState& GameState::operator=(const GameState& other) {
    if (this != &other) {
        impl_ = std::make_unique<Impl>(*other.impl_);
    }
    return *this;
}

GameState& GameState::operator=(GameState&&) noexcept = default;

GameState::~GameState() = default;

GameState GameState::startpos() {
    return {};
}

std::expected<GameState, PositionError> GameState::from_fen(std::string_view fen) {
    if (!valid_fen_syntax(fen)) {
        return std::unexpected(PositionError{PositionErrorCode::malformed_fen, "invalid FEN"});
    }

    GameState state;
    if (!state.impl_->compatibility_mirror.set_fen(fen)) {
        return std::unexpected(PositionError{PositionErrorCode::illegal_position, "illegal position"});
    }
    if (!state.impl_->native_position.set_fen(fen)) {
        return std::unexpected(PositionError{PositionErrorCode::illegal_position,
                                              "native position rejected FEN"});
    }
    return state;
}

std::string GameState::fen() const {
    return impl_->native_position.fen();
}

Color GameState::side_to_move() const noexcept {
    return impl_->native_position.side_to_move();
}

Piece GameState::piece_at(Square square) const noexcept {
    return impl_->native_position.piece_at(square);
}

int GameState::direct_static_exchange_gain(const MoveMetadata& initial) const noexcept {
    const PositionFeatures features = position_features();
    return static_exchange_gain_from_features(features, initial);
}

std::vector<Move> GameState::legal_moves() const {
    return impl_->native_position.legal_moves();
}

PositionConsistencySnapshot GameState::consistency_snapshot() const {
    PositionConsistencySnapshot snapshot;
    snapshot.native_fen = impl_->native_position.fen();
    snapshot.shadow_fen = impl_->compatibility_mirror.fen_for_comparison(impl_->native_position);
    snapshot.native_legal_moves = sorted_native_move_strings(impl_->native_position);
    snapshot.shadow_legal_moves = impl_->compatibility_mirror.legal_move_strings();
    snapshot.native_position_key = position_key();
    snapshot.shadow_position_key = impl_->compatibility_mirror.position_key();
    snapshot.native_castling_rights = castling_rights();
    snapshot.shadow_castling_rights = impl_->compatibility_mirror.castling_rights();
    snapshot.native_en_passant_square = en_passant_square();
    snapshot.shadow_en_passant_square =
        impl_->compatibility_mirror.en_passant_square_for_comparison(impl_->native_position);
    snapshot.native_halfmove_clock = halfmove_clock();
    snapshot.shadow_halfmove_clock = impl_->compatibility_mirror.halfmove_clock();
    snapshot.native_fullmove_number = fullmove_number();
    snapshot.shadow_fullmove_number = impl_->compatibility_mirror.fullmove_number();
    snapshot.native_repetition_count = repetition_count();
    snapshot.shadow_repetition_count = impl_->compatibility_mirror.repetition_count();
    snapshot.native_repetition_sensitive = is_repetition_sensitive();
    snapshot.shadow_repetition_sensitive = snapshot.shadow_repetition_count >= 2;
    snapshot.native_can_claim_threefold_repetition = can_claim_threefold_repetition();
    snapshot.native_is_automatic_fivefold_repetition = is_automatic_fivefold_repetition();
    snapshot.native_in_check = in_check();
    snapshot.shadow_in_check = impl_->compatibility_mirror.in_check();
    snapshot.native_side_to_move = side_to_move();
    snapshot.shadow_side_to_move = impl_->compatibility_mirror.side_to_move();
    const bool shadow_checkmate = snapshot.shadow_in_check && snapshot.shadow_legal_moves.empty();
    snapshot.shadow_can_claim_threefold_repetition =
        snapshot.shadow_repetition_count >= 3 && !shadow_checkmate;
    snapshot.shadow_is_automatic_fivefold_repetition =
        snapshot.shadow_repetition_count >= 5 && !shadow_checkmate;
    return snapshot;
}

bool GameState::native_shadow_consistent() const noexcept {
    return impl_->compatibility_mirror.matches(impl_->native_position);
}

std::vector<MoveMetadata> GameState::legal_moves_with_metadata() const {
    MoveMetadataList fixed_moves;
    legal_moves_with_metadata(fixed_moves);

    std::vector<MoveMetadata> moves;
    moves.reserve(fixed_moves.size());
    for (const MoveMetadata& metadata : fixed_moves) {
        moves.push_back(metadata);
    }
    return moves;
}

void GameState::legal_moves_with_metadata(MoveMetadataList& moves,
                                           bool include_check_flags,
                                           bool include_see,
                                           CheckFlagMode check_flag_mode) const noexcept {
    moves.clear();
    const std::uint64_t key = position_key();
    try {
        std::array<Move, kMaximumLegalMoves> legal{};
        const std::size_t legal_count = impl_->native_position.legal_moves_into(legal);
        for (std::size_t index = 0; index < legal_count; ++index) {
            const Move& move = legal[index];
            const auto metadata = metadata_for_native_move(move, include_check_flags,
                                                           check_flag_mode);
            if (!metadata.has_value() || !moves.push_back(*metadata)) {
                break;
            }
        }
    } catch (...) {
        moves.clear();
    }
    finalize_metadata(moves, key, include_see);
}

bool GameState::legal_tactical_moves_with_metadata(MoveMetadataList& moves,
                                                    bool include_quiet_checks,
                                                    bool include_see,
                                                    CheckFlagMode check_flag_mode) const noexcept {
    moves.clear();
    const std::uint64_t key = position_key();
    bool has_legal_moves = false;
    try {
        std::array<Move, kMaximumLegalMoves> legal{};
        const std::size_t legal_count = impl_->native_position.legal_moves_into(legal);
        has_legal_moves = legal_count != 0;
        const bool checked = impl_->native_position.in_check();
        for (std::size_t index = 0; index < legal_count; ++index) {
            const Move& move = legal[index];
            auto metadata = metadata_for_native_move(move, false, check_flag_mode);
            if (!metadata.has_value()) {
                continue;
            }

            const bool capture_or_promotion = metadata->is_capture() ||
                metadata->move.promotion() != Promotion::none;
            const bool needs_check_probe = checked || capture_or_promotion || include_quiet_checks;
            if (needs_check_probe) {
                const CheckFlagMode probe_mode = checked || capture_or_promotion ?
                    CheckFlagMode::all_moves : CheckFlagMode::quiet_moves_only;
                metadata = metadata_for_native_move(move, true, probe_mode);
            }
            if (checked || capture_or_promotion || (include_quiet_checks && metadata->gives_check)) {
                (void)moves.push_back(*metadata);
            }
        }
    } catch (...) {
        moves.clear();
    }
    finalize_metadata(moves, key, include_see);
    return has_legal_moves;
}

std::optional<MoveMetadata> GameState::describe_move(const Move& move) const noexcept {
    if (move.is_no_move() || !impl_->native_position.is_legal(move)) {
        return std::nullopt;
    }
    auto metadata = metadata_for_native_move(move, true, CheckFlagMode::all_moves);
    if (!metadata.has_value()) {
        return std::nullopt;
    }
    metadata->position_key = position_key();
    metadata->validation_token = metadata_validation_token(metadata->position_key, *metadata);
    if (metadata->is_capture() || move.promotion() != Promotion::none) {
        metadata->see_score = metadata->is_capture() ? static_cast<std::int16_t>(std::clamp(
            direct_static_exchange_gain(*metadata),
            static_cast<int>(std::numeric_limits<std::int16_t>::min()),
            static_cast<int>(std::numeric_limits<std::int16_t>::max()))) : 0;
        metadata->see_computed = true;
    }
    return metadata;
}

std::optional<MoveMetadata> GameState::metadata_for_native_move(
    const Move& move, const bool include_check_flags,
    const CheckFlagMode check_flag_mode) const noexcept {
    if (move.is_no_move() || move.from().index() >= Square::kInvalid ||
        move.to().index() >= Square::kInvalid) {
        return std::nullopt;
    }
    const Piece moving = impl_->native_position.piece_at(move.from());
    if (moving.empty() || moving.color != side_to_move()) {
        return std::nullopt;
    }

    const Piece target = impl_->native_position.piece_at(move.to());
    MoveMetadata metadata;
    metadata.move = move;
    metadata.moving_piece = moving.type;
    metadata.captured_piece = target.empty() ? PieceType::none : target.type;
    metadata.kind = MoveKind::quiet;
    if (move.promotion() != Promotion::none) {
        metadata.kind = MoveKind::promotion;
    } else if (moving.type == PieceType::king &&
               std::abs(static_cast<int>(move.to().index()) - static_cast<int>(move.from().index())) == 2) {
        metadata.kind = MoveKind::castling;
    } else if (impl_->native_position.is_capture(move)) {
        metadata.kind = target.empty() ? MoveKind::en_passant : MoveKind::capture;
        if (target.empty()) {
            metadata.captured_piece = PieceType::pawn;
        }
    }

    const bool analyze_check = include_check_flags &&
        (check_flag_mode == CheckFlagMode::all_moves ||
         (check_flag_mode == CheckFlagMode::quiet_moves_only &&
          !metadata.is_capture() && move.promotion() == Promotion::none));
    if (analyze_check) {
        impl_->check_flag_evaluations.fetch_add(1, std::memory_order_relaxed);
        if (check_flag_mode == CheckFlagMode::quiet_moves_only) {
            metadata.gives_check = native_move_gives_check(impl_->native_position, move);
        } else {
            metadata.gives_check = impl_->compatibility_mirror.gives_check(move);
        }
    }
    return metadata;
}

void GameState::finalize_metadata(MoveMetadataList& moves, const std::uint64_t key,
                                  const bool include_see,
                                  const PositionFeatures* exchange_features) const noexcept {
    std::optional<PositionFeatures> owned_features;
    if (include_see && exchange_features == nullptr) {
        const bool has_capture = std::any_of(moves.begin(), moves.end(),
            [](const MoveMetadata& metadata) { return metadata.is_capture(); });
        if (has_capture) {
            owned_features = position_features();
            exchange_features = &*owned_features;
        }
    }

    for (MoveMetadata& metadata : moves) {
        metadata.position_key = key;
        metadata.validation_token = metadata_validation_token(key, metadata);
        if (include_see && (metadata.is_capture() || metadata.move.promotion() != Promotion::none)) {
            metadata.see_score = metadata.is_capture() && exchange_features != nullptr ?
                static_cast<std::int16_t>(std::clamp(
                    static_exchange_gain_from_features(*exchange_features, metadata),
                    static_cast<int>(std::numeric_limits<std::int16_t>::min()),
                    static_cast<int>(std::numeric_limits<std::int16_t>::max()))) : 0;
            metadata.see_computed = true;
        }
    }
}

PositionFeatures GameState::position_features() const noexcept {
    // The native position already owns incremental piece bitboards.  Build
    // evaluation features from that authority instead of walking the shadow
    // chess.hpp board and asking it to recompute attacks for every piece.
    // The shadow remains synchronized for legality/tablebase compatibility;
    // it is intentionally no longer on the evaluator hot path.
    return impl_->feature_state.get_or_compute(
        impl_->compatibility_mirror.history_size(),
        impl_->native_position.position_key(),
        impl_->native_position,
        native_position_features);
}

std::uint64_t GameState::position_feature_cache_misses() const noexcept {
    return impl_->feature_state.cache_misses();
}

std::uint64_t GameState::position_feature_cache_fast_hits() const noexcept {
    return impl_->feature_state.fast_hits();
}

std::uint64_t GameState::position_feature_cache_copies() const noexcept {
    return impl_->feature_state.snapshot_copies();
}

std::uint64_t GameState::check_flag_evaluations() const noexcept {
    return impl_->check_flag_evaluations.load(std::memory_order_relaxed);
}

std::size_t TablebaseSnapshot::piece_count() const noexcept {
    std::size_t count = 0;
    for (const auto& colors : piece_bitboards) {
        for (const std::uint64_t bitboard : colors) {
            count += static_cast<std::size_t>(std::popcount(bitboard));
        }
    }
    return count;
}

TablebaseSnapshot GameState::tablebase_snapshot() const noexcept {
    TablebaseSnapshot snapshot;
    snapshot.side_to_move = side_to_move();
    snapshot.halfmove_clock = halfmove_clock();
    snapshot.castling_rights = castling_rights();
    snapshot.en_passant_square = en_passant_square();
    for (std::uint8_t square = 0; square < Square::kInvalid; ++square) {
        const Piece piece = piece_at(Square::from_index(square));
        if (piece.empty()) {
            continue;
        }
        const std::size_t color = piece.color == Color::white ? 0 : 1;
        const std::size_t type = static_cast<std::size_t>(piece.type) - 1;
        snapshot.piece_bitboards[color][type] |= std::uint64_t{1} << square;
    }
    return snapshot;
}

bool GameState::is_legal(const Move& move) const noexcept {
    return impl_->native_position.is_legal(move);
}

bool GameState::make_move(const Move& move) noexcept {
    if (!impl_->native_position.make_move(move)) {
        return false;
    }
    const std::size_t mirror_history_before =
        impl_->compatibility_mirror.history_size();
    const bool mirror_applied = impl_->compatibility_mirror.apply_move(move);
    if (!mirror_applied || !impl_->compatibility_mirror.matches(impl_->native_position)) {
        if (impl_->compatibility_mirror.history_size() > mirror_history_before) {
            (void)impl_->compatibility_mirror.undo_move();
        }
        (void)impl_->native_position.unmake_move();
        return false;
    }
    invalidate_feature_cache();
    return true;
}

bool GameState::apply_generated_move(const MoveMetadata& metadata,
                                     const bool verify_shadow_legality,
                                     const bool verify_mirror) noexcept {
    if (metadata.position_key != position_key()) {
        return false;
    }
    if (metadata.validation_token == 0 ||
        metadata.validation_token != metadata_validation_token(position_key(), metadata)) {
        return false;
    }
    if (!impl_->native_position.make_generated_move(metadata.move)) {
        return false;
    }
    const std::size_t mirror_history_before =
        impl_->compatibility_mirror.history_size();
    if (!impl_->compatibility_mirror.apply_generated_move(metadata, verify_shadow_legality)) {
        if (impl_->compatibility_mirror.history_size() > mirror_history_before) {
            (void)impl_->compatibility_mirror.undo_move();
        }
        (void)impl_->native_position.unmake_move();
        return false;
    }
    if (verify_mirror && !impl_->compatibility_mirror.matches(impl_->native_position)) {
        if (impl_->compatibility_mirror.history_size() > mirror_history_before) {
            (void)impl_->compatibility_mirror.undo_move();
        }
        (void)impl_->native_position.unmake_move();
        return false;
    }
    invalidate_feature_cache();
    return true;
}

bool GameState::make_legal_move(const MoveMetadata& metadata) noexcept {
    return apply_generated_move(metadata, true, true);
}

bool GameState::make_generated_move(const MoveMetadata& metadata) noexcept {
    return apply_generated_move(metadata, false, true);
}

bool GameState::make_search_move(const MoveMetadata& metadata) noexcept {
    // The staged picker may lazily discover that a generated capture or
    // promotion gives check. `gives_check` is part of the public metadata
    // authenticity token for ordinary generated moves, so refresh only this
    // one search-local annotation after proving that the supplied token
    // matches the original unannotated move. No other metadata mutation is
    // accepted here.
    MoveMetadata search_metadata = metadata;
    if ((metadata.is_capture() || metadata.move.promotion() != Promotion::none) &&
        metadata.gives_check) {
        MoveMetadata unannotated = metadata;
        unannotated.gives_check = false;
        const std::uint64_t key = position_key();
        if (metadata.validation_token == metadata_validation_token(key, unannotated)) {
            search_metadata.validation_token = metadata_validation_token(key, search_metadata);
        }
    }
    return apply_generated_move(search_metadata, false, false);
}

bool GameState::unmake_move() noexcept {
    if (impl_->compatibility_mirror.history_size() == 0 ||
        impl_->compatibility_mirror.last_move_is_null()) {
        return false;
    }
    if (!impl_->compatibility_mirror.undo_move()) {
        return false;
    }
    if (!impl_->native_position.unmake_move()) {
        return false;
    }
    return true;
}

bool GameState::make_null_move() noexcept {
    if (!impl_->native_position.make_null_move()) {
        return false;
    }
    if (!impl_->compatibility_mirror.apply_null_move()) {
        (void)impl_->native_position.unmake_null_move();
        return false;
    }
    invalidate_feature_cache();
    return true;
}

bool GameState::unmake_null_move() noexcept {
    if (impl_->compatibility_mirror.history_size() == 0 ||
        !impl_->compatibility_mirror.last_move_is_null()) {
        return false;
    }
    if (!impl_->compatibility_mirror.undo_null_move()) {
        return false;
    }
    if (!impl_->native_position.unmake_null_move()) {
        return false;
    }
    return true;
}

void GameState::invalidate_feature_cache() noexcept {
    impl_->feature_state.invalidate(impl_->compatibility_mirror.history_size());
}

bool GameState::is_capture(const Move& move) const noexcept {
    return impl_->native_position.is_capture(move);
}

bool GameState::move_gives_check(const Move& move) const noexcept {
    return native_move_gives_check(impl_->native_position, move);
}

bool GameState::in_check() const noexcept {
    return impl_->native_position.in_check();
}

bool GameState::in_check(Color color) const noexcept {
    return impl_->native_position.in_check(color);
}

bool GameState::has_non_pawn_material(Color color) const noexcept {
    return impl_->native_position.has_non_pawn_material(color);
}

bool GameState::is_repetition_sensitive() const noexcept {
    return impl_->native_position.is_repetition_sensitive();
}

bool GameState::is_draw_by_rule() const noexcept {
    return impl_->native_position.is_draw_by_rule();
}

bool GameState::is_terminal() const noexcept {
    return impl_->native_position.is_terminal();
}

std::uint64_t GameState::position_key() const noexcept {
    return impl_->native_position.position_key();
}

std::uint64_t GameState::pawn_key() const noexcept {
    return impl_->native_position.pawn_key();
}

std::uint64_t GameState::polyglot_key() const noexcept {
    std::uint64_t key = 0;
    for (std::uint8_t index = 0; index < 64; ++index) {
        const Piece piece = piece_at(Square::from_index(index));
        if (piece.empty()) {
            continue;
        }
        const std::size_t piece_index = (static_cast<std::size_t>(piece.type) - 1) * 2 +
            (piece.color == Color::black ? 0 : 1);
        key ^= kPolyglotRandom[piece_index * 64 + index];
    }

    const std::uint8_t rights = impl_->compatibility_mirror.castling_rights();
    if ((rights & kWhiteKingSideCastling) != 0) {
        key ^= kPolyglotRandom[768];
    }
    if ((rights & kWhiteQueenSideCastling) != 0) {
        key ^= kPolyglotRandom[769];
    }
    if ((rights & kBlackKingSideCastling) != 0) {
        key ^= kPolyglotRandom[770];
    }
    if ((rights & kBlackQueenSideCastling) != 0) {
        key ^= kPolyglotRandom[771];
    }

    const Square en_passant = impl_->compatibility_mirror.en_passant_square();
    if (en_passant.index() < Square::kInvalid) {
        const int target = en_passant.index();
        const int source_rank_delta = side_to_move() == Color::white ? -8 : 8;
        const int pawn_rank = target + source_rank_delta;
        const Color side = side_to_move();
        bool capturable = false;
        for (const int file_delta : {-1, 1}) {
            const int source = pawn_rank + file_delta;
            if (source < 0 || source >= 64 || source / 8 != pawn_rank / 8) {
                continue;
            }
            const Piece piece = piece_at(Square::from_index(static_cast<std::uint8_t>(source)));
            if (piece.type == PieceType::pawn && piece.color == side) {
                capturable = true;
                break;
            }
        }
        if (capturable) {
            key ^= kPolyglotRandom[772 + (en_passant.file() - 'a')];
        }
    }

    if (side_to_move() == Color::white) {
        key ^= kPolyglotRandom[780];
    }
    return key;
}

std::uint8_t GameState::castling_rights() const noexcept {
    return impl_->native_position.castling_rights();
}

Square GameState::en_passant_square() const noexcept {
    return impl_->native_position.en_passant_square();
}

std::uint16_t GameState::halfmove_clock() const noexcept {
    return impl_->native_position.halfmove_clock();
}

std::uint16_t GameState::fullmove_number() const noexcept {
    return impl_->native_position.fullmove_number();
}

std::size_t GameState::repetition_count() const noexcept {
    return impl_->native_position.repetition_count();
}

bool GameState::can_claim_threefold_repetition() const noexcept {
    return impl_->native_position.can_claim_threefold_repetition();
}

bool GameState::can_claim_fifty_move_draw() const noexcept {
    return impl_->native_position.can_claim_fifty_move_draw();
}

bool GameState::is_automatic_fivefold_repetition() const noexcept {
    return impl_->native_position.is_automatic_fivefold_repetition();
}

bool GameState::is_automatic_seventy_five_move_draw() const noexcept {
    return impl_->native_position.is_automatic_seventy_five_move_draw();
}

bool GameState::is_dead_position() const noexcept {
    return impl_->native_position.is_dead_position();
}

DrawStatus GameState::draw_status() const noexcept {
    return impl_->native_position.draw_status();
}

} // namespace koi
