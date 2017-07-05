//
//  MixinUser.h
//  Sample
//
//

#import <Foundation/Foundation.h>

#import "MixinWithSomething.h"

#define PROVIDE(__value__) __attribute__((annotate("__provide__ " #__value__)))

@protocol SomeDelegate<NSObject>
- (void)doSomething;
@end

typedef struct SomeStruct
{
	struct SomeStruct* next;
	int value;
} SomeStruct;

@interface MixinUser : NSObject
{
	NSDictionary* aDictionary PROVIDE(@*);
}

@property (readonly) MixinWithSomething* mws
	PROVIDE(@length -invertName -concatenateWithPrefix:suffix:);

/* Generated in the file generated/MixinUser+mixin_property_mws_MixinWithSomething.h
 @property (readonly) NSNumber* length;
 - (NSString*)invertName;
 - (NSString*)concatenateWithPrefix:(NSString*)prefix suffix:(NSString*)suffix;
*/

@property NSNumber* aNormalProperty;

@property int anScalarProperty;

@property id<SomeDelegate> aProtocoledProperty
	PROVIDE(@* -* +*);

@property SomeStruct anStructProperty;

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
*/
