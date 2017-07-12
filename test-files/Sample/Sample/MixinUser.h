//
//  MixinUser.h
//  Sample
//
//

#import "MixinWithSomething.h"

#define PROVIDE(__value__) __attribute__((annotate("__provide__ " #__value__)))

@interface MixinUser : NSObject
{
	NSDictionary* aDictionary PROVIDE(-getObject:forKey);
}

@property (readonly) MixinWithSomething* mws
	PROVIDE(@length -invertName -concatenateWithPrefix:suffix:)
	PROVIDE(-buildSomeStruct);

/* Generated in the file generated/MixinUser+mixin_property_mws_MixinWithSomething.h
 @property (readonly) NSNumber* length;
 - (NSString*)invertName;
 - (NSString*)concatenateWithPrefix:(NSString*)prefix suffix:(NSString*)suffix;
*/

@property NSNumber* aNormalProperty PROVIDE(-intValue);

@property int anScalarProperty;

// TODO: re-activate when implement support for protocols
//@property NSString<SomeDelegate>* aStringedProtocoledProperty
//	PROVIDE(@length);

//@property id<SomeDelegate> bProtocoledProperty
//	PROVIDE(@* -* +*);

@property SomeStruct aStructProperty;

@end

/**
 The property/ivar can be declarated in either header or in the implementation file.
 In case the "mixed" items must be public, a header file for the extension must be
 included by the client.
 
 TODO: how to handle properties which are of a protocoled type? '<' and '>' cannot be
 used in file names. Maybe replace with __?
 
 TODO: wildcards: @* provides all properties, -* provides all object selectors and +*
 	all class selectors
 TODO: on wildcards, do not override properties and selectors
 
 TODO: Forbid methods that start with -init to be provided with error message!
 	as well as dealloc methods and other "special" ones that make no sense to be provided.

 TODO: handle properties whose type is parameterized (like c++ template params)

 TODO: actually any type specifier can specify protocols, and id is a generic pointer (void*),
 handled differently by clang
*/
