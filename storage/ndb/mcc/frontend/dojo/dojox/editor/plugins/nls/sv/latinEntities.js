define(
({
	/* These are already handled in the default RTE
	amp:"ampersand",lt:"less-than sign",
	gt:"greater-than sign",
	nbsp:"no-break space\nnon-breaking space",
	quot:"quote",
	*/
	iexcl:"omvänt utropstecken",
	cent:"cent",
	pound:"pund",
	curren:"valuta",
	yen:"yen\nyuan",
	brvbar:"brutet lodstreck",
	sect:"stycketecken",
	uml:"trema\nblanktecken med trema",
	copy:"copyright",
	ordf:"feminin ordinal",
	laquo:"vänster dubbelt vinkelcitat\ndubbla vänsterpekande vinklar",
	not:"inte-tecken",
	shy:"mjukt bindestreck\nmjukt bindestreck",
	reg:"registrerat varumärke\nregistrerat varumärke",
	macr:"makron\nblanktecken med makron\növerstruken\nAPL-överstreck",
	deg:"grader",
	plusmn:"plus-minus\nplus- och minustecken",
	sup2:"upphöjd tvåa\nupphöjd siffra 2\nkvadrat",
	sup3:"upphöjd trea\nupphöjd siffra 3\nkubik",
	acute:"akut accent\nblanktecken med akut accent",
	micro:"mikro",
	para:"pilcrow\nstycketecken",
	middot:"mellanpunkt\ngeorgianskt komma\ngrekisk mellanpunkt",
	cedil:"cedilj\nblanktecken med cedilj",
	sup1:"upphöjd etta\nupphöjd siffra 1",
	ordm:"maskulin ordinal",
	raquo:"höger dubbelt vinkelcitat\ndubbla högerpekande vinklar",
	frac14:"Bråktal, en fjärdedel\nen fjärdedel",
	frac12:"Bråktal, en halv\nen halv",
	frac34:"Bråktal, tre fjärdedelar\ntre fjärdedelar",
	iquest:"omvänt frågetecken\nomvänt frågetecken",
	Agrave:"Latinsk versal bokstav A med grav accent\nVersalt A med grav accent",
	Aacute:"Latinsk versal bokstav A med akut accent",
	Acirc:"Latinsk versal bokstav A med cirkumflex",
	Atilde:"Latinsk versal bokstav A med tilde",
	Auml:"Latinsk versal bokstav A med trema",
	Aring:"Latinsk versal bokstav A med ring ovanför\nVersalt A med ring",
	AElig:"Latinsk versal bokstav AE\nVersalt AE",
	Ccedil:"Latinsk versal bokstav C med cedilj",
	Egrave:"Latinsk versal bokstav E med grav accent",
	Eacute:"Latinsk versal bokstav E med akut accent",
	Ecirc:"Latinsk versal bokstav E med cirkumflex",
	Euml:"Latinsk versal bokstav E med trema",
	Igrave:"Latinsk versal bokstav I med grav accent",
	Iacute:"Latinsk versal bokstav I med akut accent",
	Icirc:"Latinsk versal bokstav I med cirkumflex",
	Iuml:"Latinsk versal bokstav I med trema",
	ETH:"Latinsk versal bokstav ETH",
	Ntilde:"Latinsk versal bokstav N med tilde",
	Ograve:"Latinsk versal bokstav O med grav accent",
	Oacute:"Latinsk versal bokstav O med akut accent",
	Ocirc:"Latinsk versal bokstav O med cirkumflex",
	Otilde:"Latinsk versal bokstav O med tilde",
	Ouml:"Latinsk versal bokstav O med trema",
	times:"multiplikation",
	Oslash:"Latinsk versal bokstav O med snedstreck\nVersalt O med snedstreck",
	Ugrave:"Latinsk versal bokstav U med grav accent",
	Uacute:"Latinsk versal bokstav U med akut accent",
	Ucirc:"Latinsk versal bokstav U med cirkumflex",
	Uuml:"Latinsk versal bokstav U med trema",
	Yacute:"Latinsk versal bokstav Y med akut accent",
	THORN:"Latinsk versal bokstav THORN",
	szlig:"Latinsk gemen bokstav skarpt s\nLilla skarpa s",
	agrave:"Latinsk gemen bokstav a med grav accent\nGement a med grav accent",
	aacute:"Latinsk gemen bokstav a med akut accent",
	acirc:"Latinsk gemen bokstav a med cirkumflex",
	atilde:"Latinsk gemen bokstav a med tilde",
	auml:"Latinsk gemen bokstav a med trema",
	aring:"Latinsk gemen bokstav a med ring ovanför\nGement a med ring",
	aelig:"Latinsk gemen bokstav ae\nGement ae",
	ccedil:"Latinsk gemen bokstav c med cedilj",
	egrave:"Latinsk gemen bokstav e med grav accent",
	eacute:"Latinsk gemen bokstav e med akut accent",
	ecirc:"Latinsk gemen bokstav e med cirkumflex",
	euml:"Latinsk gemen bokstav e med trema",
	igrave:"Latinsk gemen bokstav i med grav accent",
	iacute:"Latinsk gemen bokstav i med akut accent",
	icirc:"Latinsk gemen bokstav i med cirkumflex",
	iuml:"Latinsk gemen bokstav i med trema",
	eth:"Latinsk gemen bokstav eth",
	ntilde:"Latinsk gemen bokstav n med tilde",
	ograve:"Latinsk gemen bokstav o med grav accent",
	oacute:"Latinsk gemen bokstav o med akut accent",
	ocirc:"Latinsk gemen bokstav o med cirkumflex",
	otilde:"Latinsk gemen bokstav o med tilde",
	ouml:"Latinsk gemen bokstav o med trema",
	divide:"division",
	oslash:"Latinsk gemen bokstav o med snedstreck\nGement o med snedstreck",
	ugrave:"Latinsk gemen bokstav u med grav accent",
	uacute:"Latinsk gemen bokstav u med akut accent",
	ucirc:"Latinsk gemen bokstav u med cirkumflex",
	uuml:"Latinsk gemen bokstav u med trema",
	yacute:"Latinsk gemen bokstav y med akut accent",
	thorn:"Latinsk gemen bokstav thorn",
	yuml:"Latinsk gemen bokstav y med trema",
// Greek Characters and Symbols
	fnof:"Latinsk gement f med hake\nfunktion\nflorin",
	Alpha:"Grekisk versal bokstav alfa",
	Beta:"Grekisk versal bokstav beta",
	Gamma:"Grekisk versal bokstav gamma",
	Delta:"Grekisk versal bokstav delta",
	Epsilon:"Grekisk versal bokstav epsilon",
	Zeta:"Grekisk versal bokstav zeta",
	Eta:"Grekisk versal bokstav eta",
	Theta:"Grekisk versal bokstav theta",
	Iota:"Grekisk versal bokstav iota",
	Kappa:"Grekisk versal bokstav kappa",
	Lambda:"Grekisk versal bokstav lambda",
	Mu:"Grekisk versal bokstav mu",
	Nu:"Grekisk versal bokstav nu",
	Xi:"Grekisk versal bokstav xi",
	Omicron:"Grekisk versal bokstav omikron",
	Pi:"Grekisk versal bokstav pi",
	Rho:"Grekisk versal bokstav rho",
	Sigma:"Grekisk versal bokstav sigma",
	Tau:"Grekisk versal bokstav tau",
	Upsilon:"Grekisk versal bokstav upsilon",
	Phi:"Grekisk versal bokstav phi",
	Chi:"Grekisk versal bokstav chi",
	Psi:"Grekisk versal bokstav psi",
	Omega:"Grekisk versal bokstav omega",
	alpha:"Grekisk gemen bokstav alfa",
	beta:"Grekisk gemen bokstav beta",
	gamma:"Grekisk gemen bokstav gamma",
	delta:"Grekisk gemen bokstav delta",
	epsilon:"Grekisk gemen bokstav epsilon",
	zeta:"Grekisk gemen bokstav zeta",
	eta:"Grekisk gemen bokstav eta",
	theta:"Grekisk gemen bokstav theta",
	iota:"Grekisk gemen bokstav iota",
	kappa:"Grekisk gemen bokstav kappa",
	lambda:"Grekisk gemen bokstav lambda",
	mu:"Grekisk gemen bokstav mu",
	nu:"Grekisk gemen bokstav nu",
	xi:"Grekisk gemen bokstav xi",
	omicron:"Grekisk gemen bokstav omikron",
	pi:"Grekisk gemen bokstav pi",
	rho:"Grekisk gemen bokstav rho",
	sigmaf:"Grekisk gemen bokstav avslutande sigma",
	sigma:"Grekisk gemen bokstav sigma",
	tau:"Grekisk gemen bokstav tau",
	upsilon:"Grekisk gemen bokstav upsilon",
	phi:"Grekisk gemen bokstav phi",
	chi:"Grekisk gemen bokstav chi",
	psi:"Grekisk gemen bokstav psi",
	omega:"Grekisk gemen bokstav omega",
	thetasym:"Grekisk gemen bokstav theta-symbol",
	upsih:"Grekisk upsilon med hake",
	piv:"Grekisk pi-symbol",
	bull:"punkt\nsvart liten cirkel",
	hellip:"horisontell ellips\ntre punkter",
	prime:"prim\nminuter\nfot",
	Prime:"dubbel prim\nsekunder\ntum",
	oline:"överstrykning\nblanktecken med överstrykning",
	frasl:"bråksnedstreck",
	weierp:"skrivstil versalt P\nWeierstrass-p",
	image:"gotisk versalt I\n",
	real:"gotisk versalt R\n",
	trade:"varumärke",
	alefsym:"alef-symbol\nförsta transfinita kardinalvärdet",
	larr:"vänsterpil",
	uarr:"uppåtpil",
	rarr:"högerpil",
	darr:"nedåtpil",
	harr:"dubbelpil",
	crarr:"nedåtpil med vänsterhörn\nvagnretur",
	lArr:"dubbel vänsterpil",
	uArr:"dubbel uppåtpil",
	rArr:"dubbel högerpil",
	dArr:"dubbel nedåtpil",
	hArr:"dubbel dubbelpil",
	forall:"för alla",
	part:"partiell differential",
	exist:"det finns",
	empty:"tom mängd\nnull-mängd\ndiameter",
	nabla:"nabla\nbakåtdifferens",
	isin:"element av",
	notin:"inte element av",
	ni:"innehålls som medlem",
	prod:"n-ställig produkt\nprodukt",
	sum:"n-ställig summa",
	minus:"minus",
	lowast:"asterisk",
	radic:"kvadratrot\nrotuttryck",
	prop:"proportionellt till",
	infin:"oändlighet",
	ang:"vinkel",
	and:"logiskt och\nj",
	or:"logiskt eller\n",
	cap:"snitt\n",
	cup:"union\n","int":"integral",
	there4:"därför",
	sim:"tilde-operator\nvarierar med\nliknar",
	cong:"ungefär lika med",
	asymp:"nästan lika med\nasymptotiskt till",
	ne:"inte lika med",
	equiv:"identiskt med",
	le:"mindre än eller lika med",
	ge:"större än eller lika med",
	sub:"delmängd av",
	sup:"supermängd av",
	nsub:"inte delmängd av",
	sube:"delmängd av eller lika med",
	supe:"supermängd av eller lika med",
	oplus:"plus i cirkel\ndirektsumma",
	otimes:"multiplikation i cirkel\nvektorprodukt",
	perp:"nål uppåt\nortogonalt till\nvinkelrät mot",
	sdot:"punktoperator",
	lceil:"vänstertak\nAPL upp",
	rceil:"högertak",
	lfloor:"vänstergolv\nAPL ned",
	rfloor:"högergolv",
	lang:"vänster vinkelparentes",
	rang:"höger vinkelparentes",
	loz:"romb",
	spades:"spader",
	clubs:"klöver\ntreklöver",
	hearts:"hjärter\n",
	diams:"ruter",
	OElig:"Latinsk versal ligatur OE",
	oelig:"Latinsk gemen ligatur oe",
	Scaron:"Latinsk versal bokstav S med hake",
	scaron:"Latinsk gemen bokstav s med hake",
	Yuml:"Latinsk versal bokstav Y med trema",
	circ:"cirkumflex accent",
	tilde:"litet tilde",
	ensp:"kort blanktecken",
	emsp:"långt blanktecken",
	thinsp:"tunt blanktecken",
	zwnj:"zwnj (zero width non-joiner)",
	zwj:"zwj (zero width joiner)",
	lrm:"vänster till höger-markering",
	rlm:"höger till vänster-markering",
	ndash:"kort tankstreck",
	mdash:"långt tankstreck",
	lsquo:"vänster enkelt citat",
	rsquo:"höger enkelt citat",
	sbquo:"nedre enkelt citat",
	ldquo:"vänster dubbelt citat",
	rdquo:"höger dubbelt citat",
	bdquo:"nedre dubbelt citat",
	dagger:"enkelt kors",
	Dagger:"dubbelt kors",
	permil:"promille",
	lsaquo:"vänster enkelt vinkelcitat",
	rsaquo:"höger enkelt vinkelcitat",
	euro:"euro"
})
);
